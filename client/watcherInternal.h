// Shared between watcher.c and the per-OS backends; not part of the public interface.
#ifndef DATA_SYNCHRONISATION_TOOL_WATCHER_INTERNAL_H
#define DATA_SYNCHRONISATION_TOOL_WATCHER_INTERNAL_H
#include "watcher.h"
#include "platform.h"
#include <stdatomic.h>

typedef struct WatcherBackend {
    const char* name;
    bool (*start)(struct WatcherBackend* self);
    void (*stop)(struct WatcherBackend* self);
    void (*destroy)(struct WatcherBackend* self);
    const ClientConfig* config;
    EventQueue* queue;
    ThreadHandle thread;
    atomic_bool stopRequested;
    void* state;
} WatcherBackend;

WatcherBackend* watcherPollCreate(const ClientConfig* config, EventQueue* queue);
#ifdef __linux__
WatcherBackend* watcherInotifyCreate(const ClientConfig* config, EventQueue* queue);
#endif
#ifdef _WIN32
WatcherBackend* watcherWin32Create(const ClientConfig* config, EventQueue* queue);
#endif

// Sleeps up to `milliseconds` in short slices, returning early (false) if stop was requested.
static inline bool watcherSleepUnlessStopped(WatcherBackend* backend, uint32_t milliseconds) {
    while (milliseconds > 0) {
        if (atomic_load(&backend->stopRequested)) return false;
        const uint32_t slice = milliseconds < 200 ? milliseconds : 200;
        platformSleepMs(slice);
        milliseconds -= slice;
    }
    return !atomic_load(&backend->stopRequested);
}

#endif //DATA_SYNCHRONISATION_TOOL_WATCHER_INTERNAL_H
