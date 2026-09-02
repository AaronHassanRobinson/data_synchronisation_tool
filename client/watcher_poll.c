//
// Portable polling watcher: every poll_interval_seconds, walk each root and diff (mtime, size)
// against the previous snapshot. Costs a stat per file per interval, but works on any OS and
// can't overflow. Used on macOS and wherever the native backend is disabled.
//
#include "watcherInternal.h"
#include "strmap.h"
#include "db.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t mtimeNs;
    uint64_t size;
    bool isDirectory;
    bool seenThisPass;
} Snapshot;

typedef struct {
    StrMap* snapshot; // "<root>:<relPath>" -> Snapshot*
    bool baselineTaken;
} PollState;

typedef struct {
    WatcherBackend* backend;
    PollState* state;
    uint32_t rootIndex;
} VisitContext;

static bool visitEntry(const char* fullPath, const char* relPath, const FileInfo* info, void* userData) {
    (void)fullPath;
    VisitContext* ctx = userData;
    char key[SYNC_PATH_MAX + 16];
    dbMakeKey(ctx->rootIndex, relPath, key, sizeof(key));

    Snapshot* previous = strMapGet(ctx->state->snapshot, key);
    if (!previous) {
        Snapshot* fresh = calloc(1, sizeof(Snapshot));
        fresh->mtimeNs = info->mtimeNs;
        fresh->size = info->size;
        fresh->isDirectory = info->isDirectory;
        fresh->seenThisPass = true;
        strMapPut(ctx->state->snapshot, key, fresh);
        if (ctx->state->baselineTaken) {
            eventQueuePush(ctx->backend->queue, info->isDirectory ? EVENT_DIR_CREATED : EVENT_FILE_CHANGED, ctx->rootIndex, relPath);
        }
        return true;
    }

    previous->seenThisPass = true;
    if (!info->isDirectory && (previous->mtimeNs != info->mtimeNs || previous->size != info->size)) {
        previous->mtimeNs = info->mtimeNs;
        previous->size = info->size;
        eventQueuePush(ctx->backend->queue, EVENT_FILE_CHANGED, ctx->rootIndex, relPath);
    }
    return true;
}

typedef struct { StrMap* map; char** removed; size_t count, capacity; } SweepContext;

static bool collectUnseen(const char* key, void* value, void* userData) {
    Snapshot* snapshot = value;
    SweepContext* sweep = userData;
    if (snapshot->seenThisPass) {
        snapshot->seenThisPass = false;
        return true;
    }
    if (sweep->count == sweep->capacity) {
        sweep->capacity = sweep->capacity ? sweep->capacity * 2 : 16;
        sweep->removed = realloc(sweep->removed, sweep->capacity * sizeof(char*));
    }
    sweep->removed[sweep->count++] = strdup(key);
    return true;
}

static void pollOnce(WatcherBackend* backend, PollState* state) {
    for (uint32_t i = 0; i < backend->config->rootCount; i++) {
        VisitContext ctx = { backend, state, i };
        walkDirectory(backend->config->rootPaths[i], visitEntry, &ctx);
    }
    // Deletions aren't synced (collection server), but we drop them from the snapshot so a file
    // that comes back later is reported as new.
    SweepContext sweep = { state->snapshot, NULL, 0, 0 };
    strMapForEach(state->snapshot, collectUnseen, &sweep);
    for (size_t i = 0; i < sweep.count; i++) {
        free(strMapRemove(state->snapshot, sweep.removed[i]));
        free(sweep.removed[i]);
    }
    free(sweep.removed);
    state->baselineTaken = true;
}

static void* pollThread(void* arg) {
    WatcherBackend* backend = arg;
    PollState* state = backend->state;
    pollOnce(backend, state); // baseline: the startup full scan handles the initial sync
    while (watcherSleepUnlessStopped(backend, backend->config->pollIntervalSeconds * 1000)) {
        pollOnce(backend, state);
    }
    return NULL;
}

static bool pollStart(WatcherBackend* backend) {
    return platformThreadCreate(&backend->thread, pollThread, backend);
}

static void pollStop(WatcherBackend* backend) {
    platformThreadJoin(backend->thread);
}

static void pollDestroy(WatcherBackend* backend) {
    PollState* state = backend->state;
    strMapDestroy(state->snapshot, free);
    free(state);
    free(backend);
}

WatcherBackend* watcherPollCreate(const ClientConfig* config, EventQueue* queue) {
    WatcherBackend* backend = calloc(1, sizeof(WatcherBackend));
    PollState* state = calloc(1, sizeof(PollState));
    state->snapshot = strMapCreate();
    backend->name = "poll";
    backend->start = pollStart;
    backend->stop = pollStop;
    backend->destroy = pollDestroy;
    backend->config = config;
    backend->queue = queue;
    backend->state = state;
    return backend;
}
