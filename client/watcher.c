#include "watcherInternal.h"
#include <stdlib.h>

struct Watcher {
    WatcherBackend* backend;
    bool running;
};

Watcher* watcherCreate(const ClientConfig* config, EventQueue* queue) {
    Watcher* watcher = calloc(1, sizeof(Watcher));
    if (config->watcherBackend == WATCHER_BACKEND_POLL) {
        watcher->backend = watcherPollCreate(config, queue);
    } else {
#if defined(__linux__)
        watcher->backend = watcherInotifyCreate(config, queue);
#elif defined(_WIN32)
        watcher->backend = watcherWin32Create(config, queue);
#else
        watcher->backend = watcherPollCreate(config, queue); // no native backend on this OS
#endif
    }
    return watcher;
}

bool watcherStart(Watcher* watcher) {
    if (!watcher->backend || watcher->running) return false;
    atomic_store(&watcher->backend->stopRequested, false);
    watcher->running = watcher->backend->start(watcher->backend);
    return watcher->running;
}

void watcherStop(Watcher* watcher) {
    if (!watcher->running) return;
    atomic_store(&watcher->backend->stopRequested, true);
    watcher->backend->stop(watcher->backend);
    watcher->running = false;
}

void watcherDestroy(Watcher* watcher) {
    if (!watcher) return;
    watcherStop(watcher);
    if (watcher->backend) watcher->backend->destroy(watcher->backend);
    free(watcher);
}

const char* watcherBackendName(const Watcher* watcher) {
    return watcher->backend ? watcher->backend->name : "none";
}
