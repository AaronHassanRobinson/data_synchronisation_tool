//
// The per-file / per-directory sync engine. For a file:
//   1. quick guard    - stat matches the DB record?  -> nothing to do, no hashing, no network
//   2. chunk          - stream the file through the CDC chunker (whole-file hash comes out too)
//   3. tainted check  - stat again; if it moved during the read, abandon (full scan retries later)
//   4. touch guard    - same whole-file hash as the DB?  -> update the stat triple only
//   5. manifest       - send the chunk layout; server replies with the indices it lacks
//   6. chunks + acks  - send only those, each verified and acked individually; resend any nack
//   7. finish         - server reconstructs and verifies the whole-file hash; on OK, update DB
//
#ifndef DATA_SYNCHRONISATION_TOOL_SYNC_H
#define DATA_SYNCHRONISATION_TOOL_SYNC_H
#include "config.h"
#include "db.h"
#include "session.h"

typedef enum {
    SYNC_OK = 0,
    SYNC_UNCHANGED,      // quick guard or touch guard: nothing sent
    SYNC_TAINTED,        // file changed under us; left for the next scan
    SYNC_MISSING,        // file vanished before we got to it
    SYNC_FAILED,         // server rejected / verification failed
    SYNC_DISCONNECTED,   // transport error: caller should reconnect and retry
} SyncStatus;

typedef struct {
    uint32_t chunkCount;
    uint32_t chunksSent;
    uint32_t chunksResent;
    uint64_t bytesTotal;
    uint64_t bytesSent;
} SyncFileStats;

SyncStatus syncFile(Session* session, const ClientConfig* config, StateDb* db,
                    uint32_t rootIndex, const char* relPath, SyncFileStats* stats);
SyncStatus syncDirectory(Session* session, const ClientConfig* config, StateDb* db,
                         uint32_t rootIndex, const char* relPath);
const char* syncStatusName(SyncStatus status);

#endif //DATA_SYNCHRONISATION_TOOL_SYNC_H
