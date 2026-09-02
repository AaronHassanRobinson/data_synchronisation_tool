#include "eventQueue.h"
#include "platform.h"
#include "strmap.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct Node {
    struct Node* next;
    SyncEvent event;
} Node;

struct EventQueue {
    Mutex mutex;
    CondVar available;
    Node* head;
    Node* tail;
    size_t count;
    StrMap* pending; // dedup key -> (void*)1 while an event is queued
};

EventQueue* eventQueueCreate(void) {
    EventQueue* queue = calloc(1, sizeof(EventQueue));
    platformMutexInit(&queue->mutex);
    platformCondInit(&queue->available);
    queue->pending = strMapCreate();
    return queue;
}

void eventQueueDestroy(EventQueue* queue) {
    if (!queue) return;
    for (Node* node = queue->head; node; ) {
        Node* next = node->next;
        free(node);
        node = next;
    }
    strMapDestroy(queue->pending, NULL);
    platformCondDestroy(&queue->available);
    platformMutexDestroy(&queue->mutex);
    free(queue);
}

static void makeDedupKey(EventKind kind, uint32_t rootIndex, const char* relPath, char* out, size_t capacity) {
    snprintf(out, capacity, "%d:%u:%s", (int)kind, rootIndex, relPath ? relPath : "");
}

bool eventQueuePush(EventQueue* queue, EventKind kind, uint32_t rootIndex, const char* relPath) {
    char key[SYNC_PATH_MAX + 32];
    makeDedupKey(kind, rootIndex, relPath, key, sizeof(key));

    platformMutexLock(&queue->mutex);
    if (strMapContains(queue->pending, key)) {
        platformMutexUnlock(&queue->mutex);
        return false;
    }
    Node* node = calloc(1, sizeof(Node));
    node->event.kind = kind;
    node->event.rootIndex = rootIndex;
    node->event.timestampMs = platformMonotonicMs();
    if (relPath) snprintf(node->event.relPath, sizeof(node->event.relPath), "%s", relPath);

    if (queue->tail) queue->tail->next = node; else queue->head = node;
    queue->tail = node;
    queue->count++;
    strMapPut(queue->pending, key, (void*)1);
    platformCondSignal(&queue->available);
    platformMutexUnlock(&queue->mutex);
    return true;
}

bool eventQueuePop(EventQueue* queue, SyncEvent* out, uint32_t timeoutMs) {
    platformMutexLock(&queue->mutex);
    if (!queue->head) platformCondWait(&queue->available, &queue->mutex, timeoutMs);
    Node* node = queue->head;
    if (!node) {
        platformMutexUnlock(&queue->mutex);
        return false;
    }
    queue->head = node->next;
    if (!queue->head) queue->tail = NULL;
    queue->count--;

    char key[SYNC_PATH_MAX + 32];
    makeDedupKey(node->event.kind, node->event.rootIndex, node->event.relPath, key, sizeof(key));
    strMapRemove(queue->pending, key);
    platformMutexUnlock(&queue->mutex);

    *out = node->event;
    free(node);
    return true;
}

size_t eventQueueCount(EventQueue* queue) {
    platformMutexLock(&queue->mutex);
    const size_t count = queue->count;
    platformMutexUnlock(&queue->mutex);
    return count;
}

bool batchShouldFlush(size_t pending, uint64_t lastEventMs, uint64_t nowMs, uint32_t maxEvents, uint32_t windowSeconds) {
    if (pending == 0) return false;
    if (pending >= maxEvents) return true;
    return nowMs >= lastEventMs + (uint64_t)windowSeconds * 1000;
}
