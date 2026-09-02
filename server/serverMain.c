//
// Created by MacbookPro on 22/8/2026.
//
// Server: accepts a single client connection (the design assumes a 1:1
// client/server relationship) and, for each file the client announces,
// works out which content-defined chunks it's missing, requests only
// those, reconstructs the file, and verifies it against the whole-file
// hash before writing it - this is a "collection point", so a synced file
// is never deleted or pushed back to the client.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include <protocol.h>
#include <cdc.h>
#include <cdcProtocol.h>
#include <fileUtil.h>

#define DEFAULT_PORT 9001
#define DEFAULT_OUTPUT_DIR "server_data"

// Rejects file names that could escape the output directory - the client
// only ever sends bare file names from a flat directory scan, but the
// server shouldn't trust that blindly since it's reading off the wire.
static bool isSafeFileName(const char* name, const uint32_t length) {
    if (length == 0 || length > 255) return false;
    for (uint32_t i = 0; i < length; i++) {
        if (name[i] == '/' || name[i] == '\\') return false;
    }
    if (length == 1 && name[0] == '.') return false;
    if (length == 2 && name[0] == '.' && name[1] == '.') return false;
    return true;
}

static const CdcChunkDescriptor* findChunkByHash(const CdcChunkSet* chunks, const uint8_t hash[SHA256_DIGEST_SIZE]) {
    for (uint32_t i = 0; i < chunks->chunkCount; i++) {
        if (memcmp(chunks->chunks[i].hash, hash, SHA256_DIGEST_SIZE) == 0) return &chunks->chunks[i];
    }
    return NULL;
}

typedef struct {
    uint8_t* reconstruction;
    uint32_t fileSize;
    const CdcChunkDescriptor* descriptors;
    uint32_t chunkCount;
} ChunkWriteContext;

// Places one received chunk's bytes at the offset its manifest descriptor
// says it belongs at. Bounds-checked against the descriptor rather than
// trusted blindly, since the data arrived over the wire.
static void writeChunkIntoReconstruction(const uint32_t chunkIndex, const uint8_t* data, const uint32_t length, void* userData) {
    const ChunkWriteContext* ctx = userData;
    if (chunkIndex >= ctx->chunkCount) return;
    const CdcChunkDescriptor* descriptor = &ctx->descriptors[chunkIndex];
    if (descriptor->length != length || (uint64_t)descriptor->offset + length > ctx->fileSize) return;
    memcpy(ctx->reconstruction + descriptor->offset, data, length);
}

static void handleManifest(const int clientFd, const void* payload, const uint32_t payloadLength, const char* outputDir) {
    char fileName[256];
    uint32_t fileSize = 0;
    uint8_t expectedFileHash[SHA256_DIGEST_SIZE];
    uint32_t chunkCount = 0;
    const CdcChunkDescriptor* descriptors = NULL;

    if (!parseManifestPayload(payload, payloadLength, fileName, sizeof(fileName),
                               &fileSize, expectedFileHash, &chunkCount, &descriptors)) {
        printf("  received a malformed manifest, ignoring\n");
        return;
    }
    if (!isSafeFileName(fileName, (uint32_t)strlen(fileName))) {
        printf("  refusing unsafe file name '%s'\n", fileName);
        return;
    }

    char outputPath[1024];
    snprintf(outputPath, sizeof(outputPath), "%s/%s", outputDir, fileName);
    printf("<- %s (%u bytes, %u chunks)\n", fileName, fileSize, chunkCount);

    // Chunk whatever we already have on disk for this file so we can diff
    // against it. This stands in for the design's local DB of "last known
    // server state" - here it's just recomputed from the file itself,
    // since persistence was explicitly descoped for this demo.
    uint8_t* existingData = NULL;
    size_t existingLength = 0;
    CdcChunkSet existingChunks = {0};
    if (readFileBytes(outputPath, &existingData, &existingLength)) {
        existingChunks = cdcChunkBuffer(existingData, existingLength);
    }

    uint32_t* neededIndices = malloc((chunkCount > 0 ? chunkCount : 1) * sizeof(uint32_t));
    uint32_t neededCount = 0;
    for (uint32_t i = 0; i < chunkCount; i++) {
        if (findChunkByHash(&existingChunks, descriptors[i].hash) == NULL) {
            neededIndices[neededCount++] = i;
        }
    }
    printf("   already have %u/%u chunks, requesting %u\n", chunkCount - neededCount, chunkCount, neededCount);

    uint32_t neededPayloadLength = 0;
    void* neededPayload = buildNeededChunksPayload(neededIndices, neededCount, &neededPayloadLength);
    const bool sent = sendMessage(clientFd, MSG_CDC_NEEDED_CHUNKS, neededPayload, neededPayloadLength);
    free(neededPayload);
    if (!sent) {
        printf("   failed to request chunks\n");
        goto cleanup;
    }

    {
        MessageType type;
        void* chunkDataPayload = NULL;
        uint32_t chunkDataLength = 0;
        if (!recvMessage(clientFd, &type, &chunkDataPayload, &chunkDataLength) || type != MSG_CDC_CHUNK_DATA) {
            printf("   did not receive chunk data\n");
            free(chunkDataPayload);
            goto cleanup;
        }

        uint8_t* reconstruction = calloc(fileSize > 0 ? fileSize : 1, 1);

        // Chunks the server already had a matching hash for: reuse the bytes
        // from the existing on-disk copy instead of anything off the wire.
        for (uint32_t i = 0; i < chunkCount; i++) {
            const CdcChunkDescriptor* match = findChunkByHash(&existingChunks, descriptors[i].hash);
            if (match != NULL) {
                memcpy(reconstruction + descriptors[i].offset, existingData + match->offset, descriptors[i].length);
            }
        }

        // Chunks the client just sent: copy from the received payload.
        ChunkWriteContext writeCtx = {reconstruction, fileSize, descriptors, chunkCount};
        parseChunkDataPayload(chunkDataPayload, chunkDataLength, writeChunkIntoReconstruction, &writeCtx);
        free(chunkDataPayload);

        // Two-tier verification, tier two: does the reconstructed file as a
        // whole hash to what the client's manifest claimed it should?
        uint8_t actualFileHash[SHA256_DIGEST_SIZE];
        sha256Buffer(reconstruction, fileSize, actualFileHash);
        const bool verified = memcmp(actualFileHash, expectedFileHash, SHA256_DIGEST_SIZE) == 0;

        if (verified) {
            writeFileBytesAtomic(outputPath, reconstruction, fileSize);
        }
        printf("   reconstructed + verified: %s\n", verified ? "OK, wrote file" : "HASH MISMATCH, discarded");
        free(reconstruction);

        const CdcSyncCompleteHeader completeHeader = {.success = verified ? 1 : 0};
        sendMessage(clientFd, MSG_CDC_SYNC_COMPLETE, &completeHeader, sizeof(completeHeader));
    }

    cleanup:
    free(neededIndices);
    cdcFreeChunkSet(&existingChunks);
    free(existingData);
}

int main(const int argc, char** argv) {
    printf("Data synchronisation tool - server\n");

    uint16_t port = DEFAULT_PORT;
    const char* outputDir = DEFAULT_OUTPUT_DIR;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);
    if (argc > 2) outputDir = argv[2];

    mkdir(outputDir, 0755); // fine if it already exists - we only care that it does now
    printf("Writing synced files to '%s'\n", outputDir);

    const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("socket");
        return 1;
    }

    const int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        close(serverFd);
        return 1;
    }
    if (listen(serverFd, 1) < 0) {
        perror("listen");
        close(serverFd);
        return 1;
    }
    printf("Listening on port %u...\n", port);

    socklen_t addressLength = sizeof(address);
    const int clientFd = accept(serverFd, (struct sockaddr*)&address, &addressLength);
    if (clientFd < 0) {
        perror("accept");
        close(serverFd);
        return 1;
    }
    printf("Client connected\n");

    InitialExchangeHeader initialHeader = {0};
    if (recvAll(clientFd, &initialHeader, sizeof(initialHeader))) {
        printf("Client requested protocol version %d\n", initialHeader.version);
        sendAll(clientFd, &initialHeader, sizeof(initialHeader)); // echo back to confirm support

        for (;;) {
            MessageType type;
            void* payload = NULL;
            uint32_t payloadLength = 0;
            if (!recvMessage(clientFd, &type, &payload, &payloadLength)) {
                printf("Client disconnected\n");
                break;
            }
            if (type == MSG_CDC_MANIFEST) {
                handleManifest(clientFd, payload, payloadLength, outputDir);
            } else {
                printf("Unexpected message type %d, ignoring\n", type);
            }
            free(payload);
        }
    } else {
        printf("failed to read version handshake\n");
    }

    close(clientFd);
    close(serverFd);
    return 0;
}
