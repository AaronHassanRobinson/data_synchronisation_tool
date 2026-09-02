//
// Thread-safe queue between the watcher thread(s) and the main loop, plus the batching rule.
// Pushes de-duplicate on (kind, root, path), so a file rewritten 1,000 times in a second is one
// pending event, not a thousand - this is what makes "sync after N events" mean N distinct files.
//
#ifndef DATA_SYNCHRONISATION_TOOL_EVENT_QUEUE_H
#define DATA_SYNCHRONISATION_TOOL_EVENT_QUEUE_H
#include <stdint.h>
#include <stddef.h>
#include "fileUtil.h"

typedef enum {
    EVENT_FILE_CHANGED = 1,   // created, modified, or moved in: re-chunk and sync
    EVENT_DIR_CREATED = 2,    // announce to the server, and (inotify) start watching it
    EVENT_OVERFLOW = 3,       // the OS event queue overflowed: schedule a full scan
    EVENT_FULL_SCAN = 4,      // explicit request for a full scan (timer, signal)
} EventKind;

typedef struct {
    EventKind kind;
    uint32_t rootIndex;
    char relPath[SYNC_PATH_MAX]; // '/'-separated, relative to the root
    uint64_t timestampMs;
} SyncEvent;

typedef struct EventQueue EventQueue;

EventQueue* eventQueueCreate(void);
void eventQueueDestroy(EventQueue* queue);
// Returns false if the event was a duplicate of one already pending.
bool eventQueuePush(EventQueue* queue, EventKind kind, uint32_t rootIndex, const char* relPath);
// Blocks up to timeoutMs for an event. Returns false on timeout.
bool eventQueuePop(EventQueue* queue, SyncEvent* out, uint32_t timeoutMs);
size_t eventQueueCount(EventQueue* queue);

// The batching rule from the design: flush once `pending` reaches maxEvents, or once
// windowSeconds have elapsed since the most recent event (and there is something pending).
bool batchShouldFlush(size_t pending, uint64_t lastEventMs, uint64_t nowMs, uint32_t maxEvents, uint32_t windowSeconds);

#endif //DATA_SYNCHRONISATION_TOOL_EVENT_QUEUE_H
