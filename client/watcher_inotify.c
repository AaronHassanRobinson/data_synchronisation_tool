//
// Linux inotify backend. inotify watches are per-directory and not recursive, so this backend
// adds a watch for every directory under each root at start, and for every directory it sees
// created afterwards (walking the new subtree too, since files may land in it before the watch
// is in place). IN_Q_OVERFLOW becomes EVENT_OVERFLOW so the main loop runs a full scan.
//
#ifdef __linux__
#include "watcherInternal.h"
#include "strmap.h"
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WATCH_MASK (IN_CREATE | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_DELETE_SELF | IN_ONLYDIR)

typedef struct {
    uint32_t rootIndex;
    char relDir[SYNC_PATH_MAX]; // "" for the root itself
} WatchEntry;

typedef struct {
    int fd;
    StrMap* watches; // "<wd>" -> WatchEntry*
} InotifyState;

typedef struct {
    WatcherBackend* backend;
    uint32_t rootIndex;
    bool emitEvents; // when re-walking a newly created subtree, also report its files
} AddContext;

static void addWatch(WatcherBackend* backend, uint32_t rootIndex, const char* fullPath, const char* relDir) {
    InotifyState* state = backend->state;
    const int wd = inotify_add_watch(state->fd, fullPath, WATCH_MASK);
    if (wd < 0) {
        perror("inotify_add_watch");
        return;
    }
    WatchEntry* entry = calloc(1, sizeof(WatchEntry));
    entry->rootIndex = rootIndex;
    snprintf(entry->relDir, sizeof(entry->relDir), "%s", relDir);
    char key[16];
    snprintf(key, sizeof(key), "%d", wd);
    free(strMapPut(state->watches, key, entry));
}

static bool addVisitor(const char* fullPath, const char* relPath, const FileInfo* info, void* userData) {
    AddContext* ctx = userData;
    if (info->isDirectory) {
        addWatch(ctx->backend, ctx->rootIndex, fullPath, relPath);
        if (ctx->emitEvents) eventQueuePush(ctx->backend->queue, EVENT_DIR_CREATED, ctx->rootIndex, relPath);
    } else if (ctx->emitEvents) {
        eventQueuePush(ctx->backend->queue, EVENT_FILE_CHANGED, ctx->rootIndex, relPath);
    }
    return true;
}

typedef struct {
    AddContext add;
    const char* prefix;   // relDir of the new subtree, or "" for a whole root
    size_t prefixLength;
} SubtreeFilter;

// walkDirectory always walks from the root; only act on entries inside the subtree of interest.
static bool subtreeVisitor(const char* fullPath, const char* relPath, const FileInfo* info, void* userData) {
    SubtreeFilter* filter = userData;
    if (filter->prefixLength > 0 &&
        (strncmp(relPath, filter->prefix, filter->prefixLength) != 0 || relPath[filter->prefixLength] != '/')) {
        return true;
    }
    return addVisitor(fullPath, relPath, info, &filter->add);
}

static void watchSubtree(WatcherBackend* backend, uint32_t rootIndex, const char* fullPath, const char* relDir, bool emitEvents) {
    addWatch(backend, rootIndex, fullPath, relDir);
    if (emitEvents) eventQueuePush(backend->queue, EVENT_DIR_CREATED, rootIndex, relDir);
    SubtreeFilter filter = { { backend, rootIndex, emitEvents }, relDir, strlen(relDir) };
    walkDirectory(backend->config->rootPaths[rootIndex], subtreeVisitor, &filter);
}

static void handleEvent(WatcherBackend* backend, const struct inotify_event* event) {
    InotifyState* state = backend->state;
    if (event->mask & IN_Q_OVERFLOW) {
        fprintf(stderr, "watcher: inotify queue overflowed - scheduling a full scan\n");
        eventQueuePush(backend->queue, EVENT_OVERFLOW, 0, NULL);
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), "%d", event->wd);
    WatchEntry* entry = strMapGet(state->watches, key);
    if (!entry) return;

    if (event->mask & IN_IGNORED) { // watch removed (directory deleted)
        free(strMapRemove(state->watches, key));
        return;
    }
    if (event->len == 0) return; // event about the watched directory itself (e.g. DELETE_SELF)

    char relPath[SYNC_PATH_MAX];
    const int written = entry->relDir[0]
        ? snprintf(relPath, sizeof(relPath), "%s/%s", entry->relDir, event->name)
        : snprintf(relPath, sizeof(relPath), "%s", event->name);
    if (written < 0 || (size_t)written >= sizeof(relPath)) return;

    if (event->mask & IN_ISDIR) {
        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
            char fullPath[SYNC_PATH_MAX];
            char nativeRel[SYNC_PATH_MAX];
            relPathToNative(relPath, nativeRel, sizeof(nativeRel));
            if (pathJoin(fullPath, sizeof(fullPath), backend->config->rootPaths[entry->rootIndex], nativeRel)) {
                watchSubtree(backend, entry->rootIndex, fullPath, relPath, true);
            }
        }
        return;
    }
    if (event->mask & (IN_CREATE | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO)) {
        eventQueuePush(backend->queue, EVENT_FILE_CHANGED, entry->rootIndex, relPath);
    }
}

static void* inotifyThread(void* arg) {
    WatcherBackend* backend = arg;
    InotifyState* state = backend->state;
    uint8_t buffer[64 * 1024] __attribute__((aligned(8)));

    while (!atomic_load(&backend->stopRequested)) {
        struct pollfd pfd = { .fd = state->fd, .events = POLLIN };
        const int ready = poll(&pfd, 1, 250);
        if (ready <= 0) continue;
        const ssize_t length = read(state->fd, buffer, sizeof(buffer));
        if (length <= 0) continue;
        for (ssize_t offset = 0; offset < length; ) {
            const struct inotify_event* event = (const struct inotify_event*)(buffer + offset);
            handleEvent(backend, event);
            offset += (ssize_t)(sizeof(struct inotify_event) + event->len);
        }
    }
    return NULL;
}

static bool inotifyStart(WatcherBackend* backend) {
    InotifyState* state = backend->state;
    state->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (state->fd < 0) {
        perror("inotify_init1");
        return false;
    }
    for (uint32_t i = 0; i < backend->config->rootCount; i++) {
        watchSubtree(backend, i, backend->config->rootPaths[i], "", false);
    }
    printf("watcher: inotify watching %zu directories\n", strMapCount(state->watches));
    return platformThreadCreate(&backend->thread, inotifyThread, backend);
}

static void inotifyStop(WatcherBackend* backend) {
    platformThreadJoin(backend->thread);
    InotifyState* state = backend->state;
    if (state->fd >= 0) { close(state->fd); state->fd = -1; }
}

static void inotifyDestroy(WatcherBackend* backend) {
    InotifyState* state = backend->state;
    strMapDestroy(state->watches, free);
    free(state);
    free(backend);
}

WatcherBackend* watcherInotifyCreate(const ClientConfig* config, EventQueue* queue) {
    WatcherBackend* backend = calloc(1, sizeof(WatcherBackend));
    InotifyState* state = calloc(1, sizeof(InotifyState));
    state->fd = -1;
    state->watches = strMapCreate();
    backend->name = "inotify";
    backend->start = inotifyStart;
    backend->stop = inotifyStop;
    backend->destroy = inotifyDestroy;
    backend->config = config;
    backend->queue = queue;
    backend->state = state;
    return backend;
}
#else
typedef int watcherInotifyUnusedOnThisPlatform;
#endif
