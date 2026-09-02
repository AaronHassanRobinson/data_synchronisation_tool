//
// Created by MacbookPro on 22/8/2026.
//
// Client: connects to the server, then walks a single watched directory
// (non-recursively, for this demo) and syncs every regular file in it using
// content-defined chunking - only chunks the server doesn't already have a
// matching hash for are sent over the wire.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define SJ_IMPL 1
#include <sj.h> // json library I pulled in - source mentioned in shared/sj.h

#include <protocol.h>
#include <cdc.h>
#include <cdcProtocol.h>
#include <fileUtil.h>

#define DEFAULT_CLIENT_CONFIG "client/clientConfig.json"
#define MAX_IP_SIZE 16 // "255.255.255.255" + '\0'

typedef struct {
    char serverIp[MAX_IP_SIZE];
    uint16_t serverPort;
    uint32_t scanIntervalSeconds; // parsed for config fidelity; this one-shot demo doesn't loop on it
    char directoryPath[PATH_MAX];
} ClientConfig;

// note: this is borrowed from the sj.h demo file "object.c"
static bool sjEq(const sj_Value val, const char* s) {
    const size_t len = val.end - val.start;
    return strlen(s) == len && !memcmp(s, val.start, len);
}

// Copies a sj string value into a fixed buffer, bounds-checked and
// null-terminated. sj values aren't null-terminated on their own - the
// original version of this helper didn't check length or terminate, which
// left server_port/scan_interval_seconds parsing to read uninitialized bytes.
static bool sjCopyStringZ(char* dest, const size_t destCapacity, const sj_Value val) {
    const size_t len = (size_t)(val.end - val.start);
    if (len + 1 > destCapacity) return false;
    memcpy(dest, val.start, len);
    dest[len] = '\0';
    return true;
}

static bool loadConfig(const char* path, ClientConfig* config) {
    memset(config, 0, sizeof(*config));

    uint8_t* fileData = NULL;
    size_t fileLength = 0;
    if (!readFileBytes(path, &fileData, &fileLength)) {
        printf("error reading config '%s'\n", path);
        return false;
    }

    sj_Reader reader = sj_reader((char*)fileData, fileLength);
    const sj_Value root = sj_read(&reader);
    sj_Value key, val;
    while (sj_iter_object(&reader, root, &key, &val)) {
        if (sjEq(key, "server_ip_address")) {
            sjCopyStringZ(config->serverIp, sizeof(config->serverIp), val);
        } else if (sjEq(key, "server_port")) {
            char portBuf[16] = {0};
            sjCopyStringZ(portBuf, sizeof(portBuf), val);
            config->serverPort = (uint16_t)strtol(portBuf, NULL, 10);
        } else if (sjEq(key, "scan_interval_seconds")) {
            char intervalBuf[32] = {0};
            sjCopyStringZ(intervalBuf, sizeof(intervalBuf), val);
            config->scanIntervalSeconds = (uint32_t)strtol(intervalBuf, NULL, 10);
        } else if (sjEq(key, "directory_paths")) {
            sjCopyStringZ(config->directoryPath, sizeof(config->directoryPath), val);
        }
    }
    free(fileData);

    printf("Config loaded: server=%s:%u watching='%s' (scan_interval=%us, unused by this one-shot demo)\n",
           config->serverIp, config->serverPort, config->directoryPath, config->scanIntervalSeconds);
    return true;
}

static bool isRegularFile(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Syncs one file: chunk it locally, send the manifest, send back whichever
// chunks the server says it's missing, then report the server's verification.
static bool syncFile(const int socketFd, const char* fullPath, const char* fileName) {
    bool success = false;
    uint8_t* fileData = NULL;
    size_t fileLength = 0;
    CdcChunkSet chunkSet = {0};
    void* manifestPayload = NULL;
    void* neededPayload = NULL;
    void* chunkDataPayload = NULL;
    void* completePayload = NULL;

    if (!readFileBytes(fullPath, &fileData, &fileLength)) {
        printf("  could not read %s, skipping\n", fileName);
        goto done;
    }

    chunkSet = cdcChunkBuffer(fileData, fileLength);
    char hexHash[65];
    sha256ToHex(chunkSet.fileHash, hexHash);
    printf("-> %s (%zu bytes, %u chunks, sha256 %.12s...)\n", fileName, fileLength, chunkSet.chunkCount, hexHash);

    uint32_t manifestLength = 0;
    manifestPayload = buildManifestPayload(fileName, (uint32_t)fileLength, &chunkSet, &manifestLength);
    if (!sendMessage(socketFd, MSG_CDC_MANIFEST, manifestPayload, manifestLength)) {
        printf("   failed to send manifest\n");
        goto done;
    }

    MessageType type;
    uint32_t neededLength = 0;
    if (!recvMessage(socketFd, &type, &neededPayload, &neededLength) || type != MSG_CDC_NEEDED_CHUNKS) {
        printf("   did not get a needed-chunks response\n");
        goto done;
    }

    const uint32_t* neededIndices = NULL;
    uint32_t neededCount = 0;
    if (!parseNeededChunksPayload(neededPayload, neededLength, &neededIndices, &neededCount)) {
        printf("   malformed needed-chunks response\n");
        goto done;
    }

    uint32_t chunkDataLength = 0;
    chunkDataPayload = buildChunkDataPayload(fileData, chunkSet.chunks, neededIndices, neededCount, &chunkDataLength);
    if (!sendMessage(socketFd, MSG_CDC_CHUNK_DATA, chunkDataPayload, chunkDataLength)) {
        printf("   failed to send chunk data\n");
        goto done;
    }

    printf("   sent %u/%u chunks (%.0f%% reused from server's copy)\n",
           neededCount, chunkSet.chunkCount,
           chunkSet.chunkCount ? 100.0 * (double)(chunkSet.chunkCount - neededCount) / chunkSet.chunkCount : 0.0);

    MessageType completeType;
    uint32_t completeLength = 0;
    if (!recvMessage(socketFd, &completeType, &completePayload, &completeLength)
        || completeType != MSG_CDC_SYNC_COMPLETE || completeLength < sizeof(CdcSyncCompleteHeader)) {
        printf("   did not get a completion response\n");
        goto done;
    }

    CdcSyncCompleteHeader completeHeader;
    memcpy(&completeHeader, completePayload, sizeof(completeHeader));
    printf("   server verification: %s\n", completeHeader.success ? "OK" : "FAILED");
    success = completeHeader.success != 0;

    done:
    free(manifestPayload);
    free(neededPayload);
    free(chunkDataPayload);
    free(completePayload);
    cdcFreeChunkSet(&chunkSet);
    free(fileData);
    return success;
}

int main(const int argc, char** argv) {
    printf("Data synchronisation tool - client\n");
    const char* configPath = argc > 1 ? argv[1] : DEFAULT_CLIENT_CONFIG;

    ClientConfig config;
    if (!loadConfig(configPath, &config)) return 1;

    DIR* dir = opendir(config.directoryPath);
    if (dir == NULL) {
        perror("opendir");
        printf("could not open watched directory '%s'\n", config.directoryPath);
        return 1;
    }

    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        perror("socket");
        closedir(dir);
        return 1;
    }

    struct sockaddr_in serverAddress = {0};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(config.serverPort);
    if (inet_pton(AF_INET, config.serverIp, &serverAddress.sin_addr) <= 0) {
        printf("invalid server address '%s'\n", config.serverIp);
        close(socketFd);
        closedir(dir);
        return 1;
    }

    printf("Connecting to %s:%u...\n", config.serverIp, config.serverPort);
    if (connect(socketFd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        perror("connect");
        close(socketFd);
        closedir(dir);
        return 1;
    }
    printf("Connected. Negotiating protocol version...\n");

    const InitialExchangeHeader initialHeader = {.version = PROTOCOL_VERSION_CDC_V1};
    InitialExchangeHeader initialResponse = {0};
    if (!sendAll(socketFd, &initialHeader, sizeof(initialHeader)) ||
        !recvAll(socketFd, &initialResponse, sizeof(initialResponse))) {
        printf("version handshake failed\n");
        close(socketFd);
        closedir(dir);
        return 1;
    }
    if (initialResponse.version != initialHeader.version) {
        printf("server does not support protocol version %d\n", initialHeader.version);
        close(socketFd);
        closedir(dir);
        return 1;
    }
    printf("Version %d confirmed. Syncing '%s'...\n", initialHeader.version, config.directoryPath);

    int filesSynced = 0, filesFailed = 0;
    const struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // skips ".", "..", and hidden files

        char fullPath[PATH_MAX];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", config.directoryPath, entry->d_name);
        if (!isRegularFile(fullPath)) continue;

        if (syncFile(socketFd, fullPath, entry->d_name)) filesSynced++;
        else filesFailed++;
    }
    closedir(dir);
    close(socketFd);

    printf("Done: %d file(s) synced, %d failed.\n", filesSynced, filesFailed);
    return filesFailed > 0 ? 1 : 0;
}
