#include "sync.h"
#include "cdcProtocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* syncStatusName(SyncStatus status) {
    switch (status) {
        case SYNC_OK: return "synced";
        case SYNC_UNCHANGED: return "unchanged";
        case SYNC_TAINTED: return "tainted";
        case SYNC_MISSING: return "missing";
        case SYNC_FAILED: return "failed";
        case SYNC_DISCONNECTED: return "disconnected";
        default: return "?";
    }
}

static bool fullPathFor(const ClientConfig* config, uint32_t rootIndex, const char* relPath, char* out, size_t capacity) {
    char nativeRel[SYNC_PATH_MAX];
    relPathToNative(relPath, nativeRel, sizeof(nativeRel));
    return rootIndex < config->rootCount && pathJoin(out, capacity, config->rootPaths[rootIndex], nativeRel);
}

SyncStatus syncDirectory(Session* session, const ClientConfig* config, StateDb* db, uint32_t rootIndex, const char* relPath) {
    if (dbHasDirectory(db, rootIndex, relPath)) return SYNC_UNCHANGED;

    char fullPath[SYNC_PATH_MAX];
    FileInfo info;
    if (!fullPathFor(config, rootIndex, relPath, fullPath, sizeof(fullPath)) || !statFile(fullPath, &info)) return SYNC_MISSING;
    if (!info.isDirectory) return SYNC_MISSING;

    uint32_t length = 0;
    void* payload = buildDirMetaPayload(rootIndex, relPath, info.mtimeNs / 1000000000ULL, &length);
    const bool sent = sendMessage(&session->link, MSG_DIR_META, payload, length);
    free(payload);
    if (!sent) return SYNC_DISCONNECTED;

    void* ack = NULL;
    if (!expectMessage(&session->link, MSG_DIR_META_ACK, sizeof(AckHeader), &ack, &length)) return SYNC_DISCONNECTED;
    const bool ok = ((AckHeader*)ack)->ok != 0;
    free(ack);
    if (!ok) return SYNC_FAILED;

    dbAddDirectory(db, rootIndex, relPath);
    return SYNC_OK;
}

// Sends one chunk, re-reading its bytes from disk and re-verifying the hash first: if the file
// was modified since we chunked it, the bytes no longer match the manifest and must not be sent.
static bool sendChunk(Session* session, const char* fullPath, uint32_t index, const CdcChunkDescriptor* chunk, bool* outTainted) {
    uint8_t* data = malloc(chunk->length);
    if (!readFileRange(fullPath, chunk->offset, chunk->length, data)) { free(data); *outTainted = true; return false; }
    uint8_t hash[SHA256_DIGEST_SIZE];
    sha256Buffer(data, chunk->length, hash);
    if (memcmp(hash, chunk->hash, SHA256_DIGEST_SIZE) != 0) { free(data); *outTainted = true; return false; }

    uint32_t length = 0;
    void* payload = buildChunkDataPayload(index, data, chunk->length, &length);
    free(data);
    const bool sent = sendMessage(&session->link, MSG_CHUNK_DATA, payload, length);
    free(payload);
    return sent;
}

// Sends every chunk in `indices`, collects one ack per chunk, and returns the indices that were
// nacked (for the caller to retry). Returns false on transport failure.
static bool sendChunkRound(Session* session, const char* fullPath, const CdcChunkSet* chunkSet,
                           const uint32_t* indices, uint32_t count, uint32_t* outNacked, uint32_t* outNackedCount,
                           SyncFileStats* stats, bool* outTainted) {
    uint32_t sentCount = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t index = indices[i];
        if (index >= chunkSet->chunkCount) continue; // server sent a bogus index; ignore it
        if (!sendChunk(session, fullPath, index, &chunkSet->chunks[index], outTainted)) {
            if (*outTainted) break; // stop sending; drain acks for what went out, then finish -> server reports missing
            return false;
        }
        sentCount++;
        stats->chunksSent++;
        stats->bytesSent += chunkSet->chunks[index].length;
    }

    *outNackedCount = 0;
    for (uint32_t i = 0; i < sentCount; i++) {
        void* payload = NULL;
        uint32_t length = 0;
        if (!expectMessage(&session->link, MSG_CHUNK_ACK, sizeof(ChunkAckHeader), &payload, &length)) return false;
        ChunkAckHeader ack;
        memcpy(&ack, payload, sizeof(ack));
        free(payload);
        if (!ack.ok) outNacked[(*outNackedCount)++] = ack.chunkIndex;
    }
    return true;
}

SyncStatus syncFile(Session* session, const ClientConfig* config, StateDb* db,
                    uint32_t rootIndex, const char* relPath, SyncFileStats* stats) {
    memset(stats, 0, sizeof(*stats));
    char fullPath[SYNC_PATH_MAX];
    if (!fullPathFor(config, rootIndex, relPath, fullPath, sizeof(fullPath))) return SYNC_MISSING;

    // 1. quick guard
    FileInfo before;
    if (!statFile(fullPath, &before) || before.isDirectory) return SYNC_MISSING;
    const DbFileRecord* known = dbFindFile(db, rootIndex, relPath);
    if (known && dbRecordMatchesStat(known, &before)) return SYNC_UNCHANGED;

    // 2. chunk (streaming) + 3. tainted check
    CdcChunkSet chunkSet;
    if (!cdcChunkFile(fullPath, &config->cdc, &chunkSet)) return fileExists(fullPath) ? SYNC_FAILED : SYNC_MISSING;
    FileInfo after;
    if (!statFile(fullPath, &after) || !fileInfoUnchanged(&before, &after) || chunkSet.totalLength != after.size) {
        cdcFreeChunkSet(&chunkSet);
        return SYNC_TAINTED;
    }
    stats->chunkCount = chunkSet.chunkCount;
    stats->bytesTotal = chunkSet.totalLength;

    DbFileRecord record = { .rootIndex = rootIndex, .size = after.size, .mtimeNs = after.mtimeNs, .inode = after.inode,
                            .chunkCount = chunkSet.chunkCount, .chunks = chunkSet.chunks };
    snprintf(record.relPath, sizeof(record.relPath), "%s", relPath);
    memcpy(record.fileHash, chunkSet.fileHash, SHA256_DIGEST_SIZE);

    // 4. touch guard: content identical to what the server already confirmed
    if (known && memcmp(known->fileHash, chunkSet.fileHash, SHA256_DIGEST_SIZE) == 0) {
        dbUpsertFile(db, &record);
        cdcFreeChunkSet(&chunkSet);
        return SYNC_UNCHANGED;
    }

    // 5. manifest -> needed
    SyncStatus status = SYNC_DISCONNECTED;
    uint32_t* retry = NULL;
    uint32_t* nacked = NULL;
    void* neededPayload = NULL;
    void* resultPayload = NULL;
    uint32_t length = 0;

    void* manifest = buildManifestPayload(rootIndex, relPath, after.mtimeNs / 1000000000ULL, &chunkSet, &length);
    const bool sent = sendMessage(&session->link, MSG_FILE_MANIFEST, manifest, length);
    free(manifest);
    if (!sent) goto done;

    if (!expectMessage(&session->link, MSG_FILE_NEEDED, sizeof(FileNeededHeader), &neededPayload, &length)) goto done;
    const uint32_t* needed = NULL;
    uint32_t neededCount = 0;
    if (!parseNeededPayload(neededPayload, length, &needed, &neededCount)) { status = SYNC_FAILED; goto done; }

    // 6. chunks + acks, with retries for nacked chunks
    bool tainted = false;
    retry = malloc((neededCount + 1) * sizeof(uint32_t));
    nacked = malloc((neededCount + 1) * sizeof(uint32_t));
    memcpy(retry, needed, neededCount * sizeof(uint32_t));
    uint32_t retryCount = neededCount;
    for (uint32_t round = 0; retryCount > 0 && round <= config->chunkRetryLimit; round++) {
        uint32_t nackedCount = 0;
        if (round > 0) stats->chunksResent += retryCount;
        if (!sendChunkRound(session, fullPath, &chunkSet, retry, retryCount, nacked, &nackedCount, stats, &tainted)) goto done;
        if (tainted) break;
        memcpy(retry, nacked, nackedCount * sizeof(uint32_t));
        retryCount = nackedCount;
    }

    // 7. finish -> result
    if (!sendMessage(&session->link, MSG_FILE_FINISH, NULL, 0)) goto done;
    if (!expectMessage(&session->link, MSG_FILE_RESULT, sizeof(FileResultHeader), &resultPayload, &length)) goto done;
    const FileResultHeader* result = resultPayload;
    if (tainted) {
        status = SYNC_TAINTED;
    } else if (result->success) {
        dbUpsertFile(db, &record);
        status = SYNC_OK;
    } else {
        fprintf(stderr, "sync: server reported failure (reason %u) for %s\n", result->reason, relPath);
        status = SYNC_FAILED;
    }

    done:
    free(retry);
    free(nacked);
    free(neededPayload);
    free(resultPayload);
    cdcFreeChunkSet(&chunkSet);
    return status;
}
