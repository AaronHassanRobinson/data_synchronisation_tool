#include "chunkStore.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ChunkStore {
    char directory[SYNC_PATH_MAX];
};

static void chunkPath(const ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE], char* out, size_t capacity) {
    char hex[65];
    sha256ToHex(hash, hex);
    snprintf(out, capacity, "%s%c%.2s%c%s", store->directory, PATH_SEPARATOR, hex, PATH_SEPARATOR, hex);
}

ChunkStore* chunkStoreOpen(const char* directory) {
    if (!mkdirRecursive(directory)) return NULL;
    ChunkStore* store = calloc(1, sizeof(ChunkStore));
    snprintf(store->directory, sizeof(store->directory), "%s", directory);
    return store;
}

void chunkStoreClose(ChunkStore* store) { free(store); }

bool chunkStoreHas(const ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE]) {
    char path[SYNC_PATH_MAX];
    chunkPath(store, hash, path, sizeof(path));
    return fileExists(path);
}

bool chunkStorePut(ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE], const uint8_t* data, uint32_t length) {
    uint8_t actual[SHA256_DIGEST_SIZE];
    sha256Buffer(data, length, actual);
    if (memcmp(actual, hash, SHA256_DIGEST_SIZE) != 0) return false;

    char path[SYNC_PATH_MAX];
    chunkPath(store, hash, path, sizeof(path));
    if (fileExists(path)) return true; // content-addressed: already have it

    char subdir[SYNC_PATH_MAX];
    snprintf(subdir, sizeof(subdir), "%.*s", (int)(strrchr(path, PATH_SEPARATOR) - path), path);
    if (!mkdirRecursive(subdir)) return false;
    return writeFileBytesAtomic(path, data, length);
}

bool chunkStoreRead(const ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE], uint8_t* out, uint32_t length) {
    char path[SYNC_PATH_MAX];
    chunkPath(store, hash, path, sizeof(path));
    FileInfo info;
    if (!statFile(path, &info) || info.size != length) return false;
    return readFileRange(path, 0, length, out);
}
