#include "scanner.h"

typedef struct {
    const StateDb* db;
    EventQueue* queue;
    uint32_t rootIndex;
    ScanStats* stats;
} ScanContext;

static bool scanVisitor(const char* fullPath, const char* relPath, const FileInfo* info, void* userData) {
    (void)fullPath;
    ScanContext* ctx = userData;
    if (info->isDirectory) {
        ctx->stats->directoriesSeen++;
        if (!dbHasDirectory(ctx->db, ctx->rootIndex, relPath)) {
            if (eventQueuePush(ctx->queue, EVENT_DIR_CREATED, ctx->rootIndex, relPath)) ctx->stats->directoriesQueued++;
        }
        return true;
    }
    ctx->stats->filesSeen++;
    const DbFileRecord* record = dbFindFile(ctx->db, ctx->rootIndex, relPath);
    if (!record || !dbRecordMatchesStat(record, info)) {
        if (eventQueuePush(ctx->queue, EVENT_FILE_CHANGED, ctx->rootIndex, relPath)) ctx->stats->filesQueued++;
    }
    return true;
}

ScanStats scannerFullScan(const ClientConfig* config, const StateDb* db, EventQueue* queue) {
    ScanStats stats = {0};
    for (uint32_t i = 0; i < config->rootCount; i++) {
        ScanContext ctx = { db, queue, i, &stats };
        walkDirectory(config->rootPaths[i], scanVisitor, &ctx);
    }
    return stats;
}
