//
// The client's on-disk record of the last state the server confirmed - per file: the stat
// triple used as the "quick guard" (size, mtime, inode - so a `touch` or an unchanged rewrite
// never costs a hash, let alone a transfer), the whole-file hash, and the chunk layout. Plus
// the set of directories already announced to the server.
//
// Persisted as a plain text file, rewritten atomically after every confirmed sync. A real
// deployment at scale would put this in SQLite; the format here is intentionally readable.
//
#ifndef DATA_SYNCHRONISATION_TOOL_DB_H
#define DATA_SYNCHRONISATION_TOOL_DB_H
#include <stdint.h>
#include "cdc.h"
#include "fileUtil.h"

typedef struct {
    uint32_t rootIndex;
    char relPath[SYNC_PATH_MAX];
    uint64_t size;
    uint64_t mtimeNs;
    uint64_t inode;
    uint8_t fileHash[SHA256_DIGEST_SIZE];
    uint32_t chunkCount;
    CdcChunkDescriptor* chunks; // owned by the DB
} DbFileRecord;

typedef struct StateDb StateDb;

StateDb* dbOpen(const char* path);            // loads the file if it exists; empty DB otherwise
bool dbSave(StateDb* db);                     // atomic rewrite
void dbClose(StateDb* db);
const char* dbPath(const StateDb* db);

const DbFileRecord* dbFindFile(const StateDb* db, uint32_t rootIndex, const char* relPath);
void dbUpsertFile(StateDb* db, const DbFileRecord* record); // deep-copies the chunk array
bool dbRemoveFile(StateDb* db, uint32_t rootIndex, const char* relPath);
bool dbHasDirectory(const StateDb* db, uint32_t rootIndex, const char* relPath);
void dbAddDirectory(StateDb* db, uint32_t rootIndex, const char* relPath);
size_t dbFileCount(const StateDb* db);
size_t dbDirectoryCount(const StateDb* db);

// True if the file's current stat triple matches what the DB recorded (the quick guard).
bool dbRecordMatchesStat(const DbFileRecord* record, const FileInfo* info);

// Builds the composite lookup key ("<root>:<relPath>") - exposed for the scanner/event dedup.
void dbMakeKey(uint32_t rootIndex, const char* relPath, char* out, size_t capacity);

#endif //DATA_SYNCHRONISATION_TOOL_DB_H
