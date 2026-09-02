#include "db.h"
#include "strmap.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct StateDb {
    char path[SYNC_PATH_MAX];
    StrMap* files;       // key -> DbFileRecord*
    StrMap* directories; // key -> (void*)1
};

void dbMakeKey(uint32_t rootIndex, const char* relPath, char* out, size_t capacity) {
    snprintf(out, capacity, "%u:%s", rootIndex, relPath);
}

static void freeRecord(void* value) {
    DbFileRecord* record = value;
    free(record->chunks);
    free(record);
}

static bool hexToBytes(const char* hex, uint8_t* out, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned value;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) return false;
        out[i] = (uint8_t)value;
    }
    return true;
}

static bool loadFromFile(StateDb* db) {
    FILE* file = fopen(db->path, "r");
    if (!file) return true; // no DB yet is a normal first run

    char line[SYNC_PATH_MAX + 256];
    DbFileRecord* current = NULL;
    uint32_t chunksExpected = 0;

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == 'F') {
            unsigned rootIndex, chunkCount;
            unsigned long long size, mtimeNs, inode;
            char hashHex[65];
            int consumed = 0;
            if (sscanf(line, "F %u %llu %llu %llu %64s %u %n", &rootIndex, &size, &mtimeNs, &inode, hashHex, &chunkCount, &consumed) != 6) continue;
            DbFileRecord* record = calloc(1, sizeof(DbFileRecord));
            record->rootIndex = rootIndex;
            record->size = size;
            record->mtimeNs = mtimeNs;
            record->inode = inode;
            hexToBytes(hashHex, record->fileHash, SHA256_DIGEST_SIZE);
            record->chunks = chunkCount ? calloc(chunkCount, sizeof(CdcChunkDescriptor)) : NULL;
            snprintf(record->relPath, sizeof(record->relPath), "%s", line + consumed);
            char key[SYNC_PATH_MAX + 16];
            dbMakeKey(rootIndex, record->relPath, key, sizeof(key));
            void* previous = strMapPut(db->files, key, record);
            if (previous) freeRecord(previous);
            current = record;
            chunksExpected = chunkCount;
        } else if (line[0] == 'C' && current && current->chunkCount < chunksExpected) {
            unsigned long long offset;
            unsigned length;
            char hashHex[65];
            if (sscanf(line, "C %llu %u %64s", &offset, &length, hashHex) != 3) continue;
            CdcChunkDescriptor* chunk = &current->chunks[current->chunkCount++];
            chunk->offset = offset;
            chunk->length = length;
            hexToBytes(hashHex, chunk->hash, SHA256_DIGEST_SIZE);
        } else if (line[0] == 'D') {
            unsigned rootIndex;
            int consumed = 0;
            if (sscanf(line, "D %u %n", &rootIndex, &consumed) != 1) continue;
            dbAddDirectory(db, rootIndex, line + consumed);
        }
    }
    fclose(file);
    return true;
}

StateDb* dbOpen(const char* path) {
    StateDb* db = calloc(1, sizeof(StateDb));
    snprintf(db->path, sizeof(db->path), "%s", path);
    db->files = strMapCreate();
    db->directories = strMapCreate();
    loadFromFile(db);
    return db;
}

typedef struct { FILE* file; } SaveContext;

static bool writeFileRecord(const char* key, void* value, void* userData) {
    (void)key;
    const DbFileRecord* record = value;
    FILE* file = ((SaveContext*)userData)->file;
    char hashHex[65];
    sha256ToHex(record->fileHash, hashHex);
    fprintf(file, "F %u %llu %llu %llu %s %u %s\n", record->rootIndex, (unsigned long long)record->size,
            (unsigned long long)record->mtimeNs, (unsigned long long)record->inode, hashHex, record->chunkCount, record->relPath);
    for (uint32_t i = 0; i < record->chunkCount; i++) {
        sha256ToHex(record->chunks[i].hash, hashHex);
        fprintf(file, "C %llu %u %s\n", (unsigned long long)record->chunks[i].offset, record->chunks[i].length, hashHex);
    }
    return true;
}

static bool writeDirectoryRecord(const char* key, void* value, void* userData) {
    (void)value;
    const char* colon = strchr(key, ':');
    fprintf(((SaveContext*)userData)->file, "D %.*s %s\n", (int)(colon - key), key, colon + 1);
    return true;
}

bool dbSave(StateDb* db) {
    char tmpPath[SYNC_PATH_MAX + 8];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", db->path);
    FILE* file = fopen(tmpPath, "w");
    if (!file) return false;

    fprintf(file, "# sync-state v1: F <root> <size> <mtimeNs> <inode> <fileHash> <chunkCount> <relPath> / C <offset> <length> <hash> / D <root> <relPath>\n");
    SaveContext context = { file };
    strMapForEach(db->files, writeFileRecord, &context);
    strMapForEach(db->directories, writeDirectoryRecord, &context);
    const bool ok = fflush(file) == 0 && !ferror(file);
    fclose(file);
    if (!ok || !platformRenameReplace(tmpPath, db->path)) { remove(tmpPath); return false; }
    return true;
}

void dbClose(StateDb* db) {
    if (!db) return;
    strMapDestroy(db->files, freeRecord);
    strMapDestroy(db->directories, NULL);
    free(db);
}

const char* dbPath(const StateDb* db) { return db->path; }

const DbFileRecord* dbFindFile(const StateDb* db, uint32_t rootIndex, const char* relPath) {
    char key[SYNC_PATH_MAX + 16];
    dbMakeKey(rootIndex, relPath, key, sizeof(key));
    return strMapGet(db->files, key);
}

void dbUpsertFile(StateDb* db, const DbFileRecord* source) {
    DbFileRecord* copy = malloc(sizeof(DbFileRecord));
    *copy = *source;
    copy->chunks = source->chunkCount ? malloc(source->chunkCount * sizeof(CdcChunkDescriptor)) : NULL;
    if (source->chunkCount) memcpy(copy->chunks, source->chunks, source->chunkCount * sizeof(CdcChunkDescriptor));

    char key[SYNC_PATH_MAX + 16];
    dbMakeKey(source->rootIndex, source->relPath, key, sizeof(key));
    void* previous = strMapPut(db->files, key, copy);
    if (previous) freeRecord(previous);
}

bool dbRemoveFile(StateDb* db, uint32_t rootIndex, const char* relPath) {
    char key[SYNC_PATH_MAX + 16];
    dbMakeKey(rootIndex, relPath, key, sizeof(key));
    void* removed = strMapRemove(db->files, key);
    if (removed) freeRecord(removed);
    return removed != NULL;
}

bool dbHasDirectory(const StateDb* db, uint32_t rootIndex, const char* relPath) {
    char key[SYNC_PATH_MAX + 16];
    dbMakeKey(rootIndex, relPath, key, sizeof(key));
    return strMapContains(db->directories, key);
}

void dbAddDirectory(StateDb* db, uint32_t rootIndex, const char* relPath) {
    char key[SYNC_PATH_MAX + 16];
    dbMakeKey(rootIndex, relPath, key, sizeof(key));
    strMapPut(db->directories, key, (void*)1);
}

size_t dbFileCount(const StateDb* db) { return strMapCount(db->files); }
size_t dbDirectoryCount(const StateDb* db) { return strMapCount(db->directories); }

bool dbRecordMatchesStat(const DbFileRecord* record, const FileInfo* info) {
    return record->size == info->size && record->mtimeNs == info->mtimeNs && record->inode == info->inode;
}
