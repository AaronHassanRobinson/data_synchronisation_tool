#include "serverSession.h"
#include "cdcProtocol.h"
#include "compress.h"
#include "hmac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROOTS 32

typedef struct {
    ProtocolLink link;
    const ServerConfig* config;
    ChunkStore* store;
    ServerSessionStats stats;
    char clientId[PROTOCOL_MAX_CLIENT_ID + 1];
    CdcParams cdc;
    char rootDirectories[MAX_ROOTS][SYNC_PATH_MAX];
    uint32_t rootCount;

    // The file currently being transferred (between FILE_MANIFEST and FILE_FINISH).
    bool fileActive;
    FileManifestHeader manifest;
    CdcChunkDescriptor* descriptors;
    char targetPath[SYNC_PATH_MAX];
    char relPath[SYNC_PATH_MAX];
} ServerSession;

void serverSanitizeLabel(const char* label, char* out, size_t capacity) {
    size_t n = 0;
    bool anyRealCharacter = false;
    for (size_t i = 0; label[i] && n + 1 < capacity && n < 128; i++) {
        const char c = label[i];
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out[n++] = keep ? c : '_';
        if (keep && c != '.') anyRealCharacter = true;
    }
    out[n] = '\0';
    if (!anyRealCharacter) snprintf(out, capacity, "root");
}

// ---------------------------------------------------------------- handshake

static bool handleHello(ServerSession* s) {
    void* payload = NULL;
    uint32_t length = 0;
    if (!expectMessage(&s->link, MSG_HELLO, sizeof(HelloHeader), &payload, &length)) return false;
    HelloHeader hello;
    const bool parsed = parseHelloPayload(payload, length, &hello, s->clientId, sizeof(s->clientId));
    free(payload);
    if (!parsed) return false;

    HelloAckHeader ack = {0};
    // Highest version both sides support, or 0 if the ranges don't overlap.
    const uint16_t low = hello.minVersion > PROTOCOL_VERSION_MIN ? hello.minVersion : PROTOCOL_VERSION_MIN;
    const uint16_t high = hello.maxVersion < PROTOCOL_VERSION_MAX ? hello.maxVersion : PROTOCOL_VERSION_MAX;
    ack.chosenVersion = low <= high ? high : 0;
    ack.capabilities = hello.capabilities & (compressionAvailable() ? CAP_ZSTD : 0);
    s->cdc = cdcParamsFromWire(&hello.cdc);
    if (!cdcParamsValid(&s->cdc) || !serverConfigClientAllowed(s->config, s->clientId)) ack.chosenVersion = 0;
    if (!randomBytes(ack.nonce, PROTOCOL_NONCE_SIZE)) return false;

    if (!sendMessage(&s->link, MSG_HELLO_ACK, &ack, sizeof(ack))) return false;
    if (ack.chosenVersion == 0) {
        printf("session: rejected client '%s' (versions %u-%u, cdc %u/%u/%u)\n", s->clientId, hello.minVersion,
               hello.maxVersion, s->cdc.minChunkSize, s->cdc.maxChunkSize, s->cdc.maskBits);
        return false;
    }
    s->link.compressionEnabled = (ack.capabilities & CAP_ZSTD) != 0;

    // --- AUTH: verify the client's MAC over our nonce, then answer their nonce ---
    if (!expectMessage(&s->link, MSG_AUTH, sizeof(AuthHeader), &payload, &length)) return false;
    AuthHeader auth;
    memcpy(&auth, payload, sizeof(auth));
    free(payload);

    uint8_t message[PROTOCOL_NONCE_SIZE + PROTOCOL_MAX_CLIENT_ID];
    memcpy(message, ack.nonce, PROTOCOL_NONCE_SIZE);
    const size_t idLength = strlen(s->clientId);
    memcpy(message + PROTOCOL_NONCE_SIZE, s->clientId, idLength);
    uint8_t expected[SHA256_DIGEST_SIZE];
    const uint8_t* psk = (const uint8_t*)s->config->preSharedKey;
    hmacSha256(psk, strlen(s->config->preSharedKey), message, PROTOCOL_NONCE_SIZE + idLength, expected);

    AuthAckHeader authAck = {0};
    authAck.success = constantTimeEquals(expected, auth.mac, SHA256_DIGEST_SIZE) ? 1 : 0;
    if (authAck.success) {
        uint8_t reply[PROTOCOL_NONCE_SIZE + 6];
        memcpy(reply, auth.clientNonce, PROTOCOL_NONCE_SIZE);
        memcpy(reply + PROTOCOL_NONCE_SIZE, "server", 6);
        hmacSha256(psk, strlen(s->config->preSharedKey), reply, sizeof(reply), authAck.mac);
    }
    if (!sendMessage(&s->link, MSG_AUTH_ACK, &authAck, sizeof(authAck))) return false;
    if (!authAck.success) {
        printf("session: authentication FAILED for client '%s'\n", s->clientId);
        return false;
    }
    printf("session: client '%s' authenticated (protocol v%u, compression %s, cdc %u/%u/%u)\n", s->clientId,
           ack.chosenVersion, s->link.compressionEnabled ? "on" : "off", s->cdc.minChunkSize, s->cdc.maxChunkSize, s->cdc.maskBits);
    return true;
}

static bool registerRoot(uint32_t index, const char* label, void* userData) {
    ServerSession* s = userData;
    if (index >= MAX_ROOTS) return false;
    char safe[160];
    serverSanitizeLabel(label, safe, sizeof(safe));

    // Two roots with the same basename (e.g. /a/data and /b/data) get distinct directories.
    char candidate[SYNC_PATH_MAX];
    for (uint32_t suffix = 0; ; suffix++) {
        char name[200];
        if (suffix == 0) snprintf(name, sizeof(name), "%s", safe);
        else snprintf(name, sizeof(name), "%s_%u", safe, suffix);
        if (!pathJoin(candidate, sizeof(candidate), s->config->outputDirectory, name)) return false;
        bool taken = false;
        for (uint32_t i = 0; i < index; i++) taken = taken || strcmp(s->rootDirectories[i], candidate) == 0;
        if (!taken) break;
    }
    snprintf(s->rootDirectories[index], SYNC_PATH_MAX, "%s", candidate);
    s->rootCount = index + 1;
    printf("session: root[%u] '%s' -> %s\n", index, label, candidate);
    return mkdirRecursive(candidate);
}

static bool handleWatchRoots(ServerSession* s) {
    void* payload = NULL;
    uint32_t length = 0;
    if (!expectMessage(&s->link, MSG_WATCH_ROOTS, sizeof(WatchRootsHeader), &payload, &length)) return false;
    const AckHeader ack = { .ok = parseWatchRootsPayload(payload, length, registerRoot, s) ? 1 : 0 };
    free(payload);
    return sendMessage(&s->link, MSG_WATCH_ROOTS_ACK, &ack, sizeof(ack)) && ack.ok;
}

// ---------------------------------------------------------------- transfer

static bool resolveTarget(const ServerSession* s, uint32_t rootIndex, const char* relPath, char* out, size_t capacity) {
    if (rootIndex >= s->rootCount || !isSafeRelativePath(relPath)) return false;
    char nativeRel[SYNC_PATH_MAX];
    relPathToNative(relPath, nativeRel, sizeof(nativeRel));
    return pathJoin(out, capacity, s->rootDirectories[rootIndex], nativeRel);
}

static bool handleDirMeta(ServerSession* s, const void* payload, uint32_t length) {
    DirMetaHeader header;
    char relPath[SYNC_PATH_MAX];
    char target[SYNC_PATH_MAX];
    AckHeader ack = { .ok = 0 };
    if (parseDirMetaPayload(payload, length, &header, relPath, sizeof(relPath)) &&
        resolveTarget(s, header.rootIndex, relPath, target, sizeof(target))) {
        ack.ok = mkdirRecursive(target) ? 1 : 0;
        printf("  dir  %s%s\n", relPath, ack.ok ? "" : " (FAILED)");
    } else {
        printf("  dir  <rejected>\n");
    }
    return sendMessage(&s->link, MSG_DIR_META_ACK, &ack, sizeof(ack));
}

// If a previous version of the file already exists on disk, chunk it with the client's
// parameters and seed the store, so unchanged regions are "already had" rather than re-sent.
static void seedStoreFromExisting(ServerSession* s) {
    FileInfo info;
    if (!statFile(s->targetPath, &info) || info.isDirectory) return;
    CdcChunkSet existing;
    if (!cdcChunkFile(s->targetPath, &s->cdc, &existing)) return;
    for (uint32_t i = 0; i < existing.chunkCount; i++) {
        if (chunkStoreHas(s->store, existing.chunks[i].hash)) continue;
        uint8_t* data = malloc(existing.chunks[i].length);
        if (readFileRange(s->targetPath, existing.chunks[i].offset, existing.chunks[i].length, data)) {
            chunkStorePut(s->store, existing.chunks[i].hash, data, existing.chunks[i].length);
        }
        free(data);
    }
    cdcFreeChunkSet(&existing);
}

static void clearActiveFile(ServerSession* s) {
    free(s->descriptors);
    s->descriptors = NULL;
    s->fileActive = false;
}

static bool handleManifest(ServerSession* s, const void* payload, uint32_t length) {
    clearActiveFile(s);
    const CdcChunkDescriptor* descriptors = NULL;
    if (!parseManifestPayload(payload, length, &s->manifest, s->relPath, sizeof(s->relPath), &descriptors) ||
        !resolveTarget(s, s->manifest.rootIndex, s->relPath, s->targetPath, sizeof(s->targetPath))) {
        printf("  file <rejected manifest>\n");
        uint32_t neededLength = 0;
        void* needed = buildNeededPayload(NULL, 0, &neededLength); // nothing needed; FINISH will fail with REJECTED
        const bool sent = sendMessage(&s->link, MSG_FILE_NEEDED, needed, neededLength);
        free(needed);
        return sent;
    }
    s->descriptors = malloc((s->manifest.chunkCount + 1) * sizeof(CdcChunkDescriptor));
    memcpy(s->descriptors, descriptors, s->manifest.chunkCount * sizeof(CdcChunkDescriptor));
    s->fileActive = true;

    seedStoreFromExisting(s);

    uint32_t* needed = malloc((s->manifest.chunkCount + 1) * sizeof(uint32_t));
    uint32_t neededCount = 0;
    uint64_t neededBytes = 0;
    for (uint32_t i = 0; i < s->manifest.chunkCount; i++) {
        if (!chunkStoreHas(s->store, s->descriptors[i].hash)) {
            needed[neededCount++] = i;
            neededBytes += s->descriptors[i].length;
        }
    }
    s->stats.bytesReused += s->manifest.fileSize - neededBytes;
    printf("  file %s: %llu bytes, %u chunks, already have %u, requesting %u (%llu bytes)\n", s->relPath,
           (unsigned long long)s->manifest.fileSize, s->manifest.chunkCount, s->manifest.chunkCount - neededCount,
           neededCount, (unsigned long long)neededBytes);

    uint32_t neededLength = 0;
    void* neededPayload = buildNeededPayload(needed, neededCount, &neededLength);
    free(needed);
    const bool sent = sendMessage(&s->link, MSG_FILE_NEEDED, neededPayload, neededLength);
    free(neededPayload);
    return sent;
}

static bool handleChunkData(ServerSession* s, const void* payload, uint32_t length) {
    uint32_t index = 0;
    const uint8_t* data = NULL;
    uint32_t dataLength = 0;
    ChunkAckHeader ack = { .chunkIndex = UINT32_MAX, .ok = 0 };

    if (parseChunkDataPayload(payload, length, &index, &data, &dataLength)) {
        ack.chunkIndex = index;
        // Tier-one verification: the chunk's bytes must hash to what the manifest promised.
        if (s->fileActive && index < s->manifest.chunkCount && dataLength == s->descriptors[index].length &&
            chunkStorePut(s->store, s->descriptors[index].hash, data, dataLength)) {
            ack.ok = 1;
            s->stats.chunksReceived++;
            s->stats.bytesReceived += dataLength;
        }
    }
    if (!ack.ok) {
        s->stats.chunksRejected++;
        printf("    chunk %u REJECTED (hash mismatch or out of range) - client will resend\n", ack.chunkIndex);
    }
    return sendMessage(&s->link, MSG_CHUNK_ACK, &ack, sizeof(ack));
}

static FileResultReason reconstruct(ServerSession* s) {
    char tmpPath[SYNC_PATH_MAX];
    if (snprintf(tmpPath, sizeof(tmpPath), "%s.partial", s->targetPath) >= (int)sizeof(tmpPath)) return FILE_RESULT_WRITE_FAILED;

    // Files can arrive before their parent's DIR_META: create the parent from the path.
    char parent[SYNC_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", s->targetPath);
    char* lastSeparator = strrchr(parent, PATH_SEPARATOR);
    if (lastSeparator) { *lastSeparator = '\0'; mkdirRecursive(parent); }

    FILE* out = fopen(tmpPath, "wb");
    if (!out) return FILE_RESULT_WRITE_FAILED;

    Sha256Context hash;
    sha256Init(&hash);
    FileResultReason reason = FILE_RESULT_OK;
    uint8_t* buffer = malloc(s->cdc.maxChunkSize);
    for (uint32_t i = 0; i < s->manifest.chunkCount && reason == FILE_RESULT_OK; i++) {
        const CdcChunkDescriptor* chunk = &s->descriptors[i];
        if (!chunkStoreRead(s->store, chunk->hash, buffer, chunk->length)) reason = FILE_RESULT_MISSING_CHUNKS;
        else if (fwrite(buffer, 1, chunk->length, out) != chunk->length) reason = FILE_RESULT_WRITE_FAILED;
        else sha256Update(&hash, buffer, chunk->length);
    }
    free(buffer);
    const bool closed = fclose(out) == 0;

    if (reason == FILE_RESULT_OK) {
        // Tier-two verification: the reassembled whole must hash to the manifest's file hash.
        uint8_t actual[SHA256_DIGEST_SIZE];
        sha256Final(&hash, actual);
        if (memcmp(actual, s->manifest.fileHash, SHA256_DIGEST_SIZE) != 0) reason = FILE_RESULT_HASH_MISMATCH;
        else if (!closed || !platformRenameReplace(tmpPath, s->targetPath)) reason = FILE_RESULT_WRITE_FAILED;
    }
    if (reason != FILE_RESULT_OK) deleteFile(tmpPath);
    return reason;
}

static bool handleFinish(ServerSession* s) {
    FileResultHeader result = { .success = 0, .reason = FILE_RESULT_REJECTED };
    if (s->fileActive) {
        result.reason = (uint8_t)reconstruct(s);
        result.success = result.reason == FILE_RESULT_OK;
        static const char* reasonNames[] = { "ok", "missing chunks", "whole-file hash mismatch", "write failed", "rejected" };
        printf("  file %s: %s\n", s->relPath, result.success ? "verified + written" : reasonNames[result.reason]);
        if (result.success) s->stats.filesReceived++; else s->stats.filesFailed++;
    }
    clearActiveFile(s);
    return sendMessage(&s->link, MSG_FILE_RESULT, &result, sizeof(result));
}

// ---------------------------------------------------------------- driver

ServerSessionStats serverSessionRun(SocketFd fd, const ServerConfig* config, ChunkStore* store) {
    ServerSession s = { .config = config, .store = store };
    platformSetSocketTimeout(fd, config->socketTimeoutMs);

    Transport* transport;
    if (config->useTls) {
        const TlsServerConfig tls = { config->tlsCertificatePath, config->tlsPrivateKeyPath };
        transport = transportTlsServer(fd, &tls);
        if (!transport) { platformCloseSocket(fd); return s.stats; }
    } else {
        transport = transportPlain(fd);
    }
    s.link.transport = transport;

    if (handleHello(&s) && handleWatchRoots(&s)) {
        for (;;) {
            MessageType type;
            void* payload = NULL;
            uint32_t length = 0;
            if (!recvMessage(&s.link, &type, &payload, &length)) {
                printf("session: client '%s' disconnected\n", s.clientId);
                break;
            }
            bool ok = true;
            switch (type) {
                case MSG_DIR_META:      ok = handleDirMeta(&s, payload, length); break;
                case MSG_FILE_MANIFEST: ok = handleManifest(&s, payload, length); break;
                case MSG_CHUNK_DATA:    ok = handleChunkData(&s, payload, length); break;
                case MSG_FILE_FINISH:   ok = handleFinish(&s); break;
                case MSG_BYE:           ok = false; printf("session: client '%s' said goodbye\n", s.clientId); break;
                default:
                    printf("session: unexpected %s from '%s', dropping connection\n", messageTypeName(type), s.clientId);
                    ok = false;
            }
            free(payload);
            if (!ok) break;
        }
    }
    clearActiveFile(&s);
    transportClose(transport);
    printf("session: %u files received, %u failed, %u chunks (%llu bytes) received, %llu bytes reused\n",
           s.stats.filesReceived, s.stats.filesFailed, s.stats.chunksReceived,
           (unsigned long long)s.stats.bytesReceived, (unsigned long long)s.stats.bytesReused);
    return s.stats;
}
