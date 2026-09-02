//
// Client entry point: the main loop with its event queue.
//
//   sync_client [config.json] [--once]
//
// Startup: load config, open the state DB, connect + authenticate, start the watcher, and run
// a full scan (this catches everything that changed while the process wasn't running - the
// design's answer to "the box restarted" is a cron/systemd/launchd/schtasks wrapper that
// restarts us, and this scan). Then loop: drain watcher events into a pending batch, flush the
// batch when the batching rule says so, run the periodic full scan, and reconnect with backoff
// whenever the transport drops (pending work is kept, not lost).
//
// --once: connect, full scan, flush everything, exit. Used by the integration tests and cron.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>

#include "config.h"
#include "db.h"
#include "eventQueue.h"
#include "scanner.h"
#include "session.h"
#include "sync.h"
#include "watcher.h"

#define DEFAULT_CLIENT_CONFIG "client/clientConfig.json"
#define POP_TIMEOUT_MS 250

static atomic_bool stopRequested = false;
static void onSignal(int signal) { (void)signal; atomic_store(&stopRequested, true); }

typedef struct {
    SyncEvent* events;
    size_t count, capacity;
    uint64_t lastEventMs;
} Batch;

static void batchAdd(Batch* batch, const SyncEvent* event) {
    for (size_t i = 0; i < batch->count; i++) { // dedup within the batch too
        if (batch->events[i].kind == event->kind && batch->events[i].rootIndex == event->rootIndex &&
            strcmp(batch->events[i].relPath, event->relPath) == 0) return;
    }
    if (batch->count == batch->capacity) {
        batch->capacity = batch->capacity ? batch->capacity * 2 : 64;
        batch->events = realloc(batch->events, batch->capacity * sizeof(SyncEvent));
    }
    batch->events[batch->count++] = *event;
    batch->lastEventMs = platformMonotonicMs();
}

typedef struct {
    const ClientConfig* config;
    StateDb* db;
    EventQueue* queue;
    Session session;
    uint64_t lastScanMs;
    bool scanPending;
} Client;

static bool ensureConnected(Client* client, bool once) {
    if (client->session.connected) return true;
    uint32_t attempt = 0;
    while (!atomic_load(&stopRequested)) {
        if (sessionConnect(&client->session, client->config)) return true;
        attempt++;
        if (once && attempt >= 3) return false;
        printf("client: connection failed (attempt %u), retrying in %us\n", attempt, client->config->reconnectDelaySeconds);
        for (uint32_t waited = 0; waited < client->config->reconnectDelaySeconds * 1000 && !atomic_load(&stopRequested); waited += 200) {
            platformSleepMs(200);
        }
    }
    return false;
}

static void runFullScan(Client* client) {
    const ScanStats stats = scannerFullScan(client->config, client->db, client->queue);
    printf("scan: %u files (%u queued), %u directories (%u queued)\n",
           stats.filesSeen, stats.filesQueued, stats.directoriesSeen, stats.directoriesQueued);
    client->lastScanMs = platformMonotonicMs();
    client->scanPending = false;
}

// Directories first so the server can create them before files land (files whose parent
// directory wasn't announced still work - the server mkdir -p's from the path - but this keeps
// the metadata records in the natural order).
static int eventOrder(const void* a, const void* b) {
    const SyncEvent* ea = a;
    const SyncEvent* eb = b;
    if (ea->kind != eb->kind) return ea->kind == EVENT_DIR_CREATED ? -1 : 1;
    return strcmp(ea->relPath, eb->relPath);
}

// Returns false if the connection dropped part-way (unprocessed events stay in the batch).
static bool flushBatch(Client* client, Batch* batch) {
    if (batch->count == 0) return true;
    qsort(batch->events, batch->count, sizeof(SyncEvent), eventOrder);
    printf("batch: flushing %zu event(s)\n", batch->count);

    uint32_t synced = 0, unchanged = 0, skipped = 0;
    uint64_t bytesSent = 0, bytesTotal = 0;
    size_t processed = 0;
    bool connectionOk = true;
    while (processed < batch->count && connectionOk && !atomic_load(&stopRequested)) {
        const SyncEvent* event = &batch->events[processed];
        SyncStatus status;
        if (event->kind == EVENT_DIR_CREATED) {
            status = syncDirectory(&client->session, client->config, client->db, event->rootIndex, event->relPath);
            if (status == SYNC_OK) printf("  dir  %s\n", event->relPath);
        } else {
            SyncFileStats stats;
            status = syncFile(&client->session, client->config, client->db, event->rootIndex, event->relPath, &stats);
            if (status == SYNC_OK) {
                const uint32_t reused = stats.chunkCount - stats.chunksSent + stats.chunksResent;
                printf("  file %s: %llu bytes, %u chunks, sent %u (%llu bytes, %u resent), server already had %u\n",
                       event->relPath, (unsigned long long)stats.bytesTotal, stats.chunkCount, stats.chunksSent,
                       (unsigned long long)stats.bytesSent, stats.chunksResent, reused);
                bytesSent += stats.bytesSent;
                bytesTotal += stats.bytesTotal;
            } else if (status != SYNC_UNCHANGED && status != SYNC_DISCONNECTED) {
                printf("  file %s: %s\n", event->relPath, syncStatusName(status));
            }
        }
        if (status == SYNC_DISCONNECTED) {
            connectionOk = false; // leave `processed` pointing at this event so it is retried
            break;
        }
        if (status == SYNC_OK) synced++;
        else if (status == SYNC_UNCHANGED) unchanged++;
        else skipped++;
        processed++;
    }

    dbSave(client->db);
    if (!connectionOk) {
        // Keep everything from the failed event onward for the retry after reconnecting.
        memmove(batch->events, batch->events + processed, (batch->count - processed) * sizeof(SyncEvent));
        batch->count -= processed;
        printf("batch: connection lost, %zu event(s) kept for retry\n", batch->count);
        client->session.connected = false; // don't try to send BYE over a dead link
        sessionClose(&client->session);
        return false;
    }
    batch->count = 0;
    printf("batch: done - %u synced, %u unchanged, %u skipped; %llu of %llu bytes sent\n",
           synced, unchanged, skipped, (unsigned long long)bytesSent, (unsigned long long)bytesTotal);
    return true;
}

int main(int argc, char** argv) {
    const char* configPath = DEFAULT_CLIENT_CONFIG;
    bool once = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) once = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [config.json] [--once]\n", argv[0]);
            return 0;
        }
        else configPath = argv[i];
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // a peer that vanished mid-write is a return code, not a death sentence
#endif

    setvbuf(stdout, NULL, _IOLBF, 0); // line-buffered even when logging to a file
    printf("Data synchronisation tool - client\n");
    ClientConfig config;
    char error[256];
    if (!clientConfigLoad(configPath, &config, error, sizeof(error))) {
        fprintf(stderr, "config: %s\n", error);
        return 1;
    }
    clientConfigPrint(&config);
    if (!platformNetInit()) return 1;

    Client client = { .config = &config, .db = dbOpen(config.databasePath), .queue = eventQueueCreate() };
    printf("db: %s (%zu files, %zu directories known)\n", config.databasePath, dbFileCount(client.db), dbDirectoryCount(client.db));

    Watcher* watcher = NULL;
    if (!once) {
        watcher = watcherCreate(&config, client.queue);
        if (!watcherStart(watcher)) {
            fprintf(stderr, "watcher: failed to start %s backend\n", watcherBackendName(watcher));
            return 1;
        }
        printf("watcher: %s backend running\n", watcherBackendName(watcher));
    }

    Batch batch = {0};
    client.scanPending = true;
    int exitCode = 0;

    while (!atomic_load(&stopRequested)) {
        if (!ensureConnected(&client, once)) {
            if (once) { fprintf(stderr, "client: could not reach the server\n"); exitCode = 2; }
            break;
        }

        const uint64_t now = platformMonotonicMs();
        if (client.scanPending || (config.scanIntervalSeconds > 0 && now >= client.lastScanMs + (uint64_t)config.scanIntervalSeconds * 1000)) {
            runFullScan(&client);
        }

        SyncEvent event;
        while (eventQueuePop(client.queue, &event, batch.count == 0 ? POP_TIMEOUT_MS : 0)) {
            if (event.kind == EVENT_OVERFLOW || event.kind == EVENT_FULL_SCAN) client.scanPending = true;
            else batchAdd(&batch, &event);
        }

        // In --once mode there is no watcher, so no batching window: flush as soon as the scan's
        // events are in, then leave once nothing is pending.
        const bool flushNow = once ? batch.count > 0
            : batchShouldFlush(batch.count, batch.lastEventMs, platformMonotonicMs(), config.batchMaxEvents, config.batchWindowSeconds);
        if (flushNow) {
            flushBatch(&client, &batch);
        } else if (batch.count == 0 && !client.scanPending) {
            if (once) { printf("batch: nothing to sync\n"); break; }
            platformSleepMs(50);
        }
    }

    printf("client: shutting down\n");
    if (watcher) watcherDestroy(watcher);
    if (batch.count > 0) printf("client: %zu pending event(s) will be picked up by the next startup scan\n", batch.count);
    sessionClose(&client.session);
    dbSave(client.db);
    dbClose(client.db);
    eventQueueDestroy(client.queue);
    free(batch.events);
    platformNetShutdown();
    return exitCode;
}
