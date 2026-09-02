//
// Windows ReadDirectoryChangesW backend. One overlapped read per root (it's natively recursive
// with bWatchSubtree), all multiplexed onto a single thread with WaitForMultipleObjects. A
// zero-length completion means the OS buffer overflowed and events were dropped - reported as
// EVENT_OVERFLOW so the main loop runs a full scan.
//
// NOTE: written against the Win32 API and compile-checked with mingw-w64, but not executed on
// a Windows host as part of this branch's test run.
//
#ifdef _WIN32
#include "watcherInternal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFY_BUFFER_SIZE (64 * 1024)
#define NOTIFY_FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | \
                       FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION)

typedef struct {
    HANDLE directory;
    OVERLAPPED overlapped;
    uint8_t* buffer;
    uint32_t rootIndex;
} RootWatch;

typedef struct {
    RootWatch roots[CONFIG_MAX_ROOTS];
    uint32_t rootCount;
    HANDLE stopEvent;
} Win32State;

static bool issueRead(RootWatch* watch) {
    return ReadDirectoryChangesW(watch->directory, watch->buffer, NOTIFY_BUFFER_SIZE, TRUE, NOTIFY_FILTER,
                                 NULL, &watch->overlapped, NULL) != 0;
}

static void handleNotifications(WatcherBackend* backend, RootWatch* watch, DWORD bytes) {
    if (bytes == 0) {
        fprintf(stderr, "watcher: ReadDirectoryChangesW buffer overflowed - scheduling a full scan\n");
        eventQueuePush(backend->queue, EVENT_OVERFLOW, 0, NULL);
        return;
    }
    const uint8_t* cursor = watch->buffer;
    for (;;) {
        const FILE_NOTIFY_INFORMATION* info = (const FILE_NOTIFY_INFORMATION*)cursor;
        char relPath[SYNC_PATH_MAX];
        const int length = WideCharToMultiByte(CP_UTF8, 0, info->FileName, (int)(info->FileNameLength / sizeof(WCHAR)),
                                               relPath, (int)sizeof(relPath) - 1, NULL, NULL);
        if (length > 0) {
            relPath[length] = '\0';
            for (char* p = relPath; *p; p++) if (*p == '\\') *p = '/';

            if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_MODIFIED ||
                info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                char nativeRel[SYNC_PATH_MAX], fullPath[SYNC_PATH_MAX];
                relPathToNative(relPath, nativeRel, sizeof(nativeRel));
                FileInfo fileInfo;
                if (pathJoin(fullPath, sizeof(fullPath), backend->config->rootPaths[watch->rootIndex], nativeRel) &&
                    statFile(fullPath, &fileInfo)) {
                    eventQueuePush(backend->queue, fileInfo.isDirectory ? EVENT_DIR_CREATED : EVENT_FILE_CHANGED,
                                   watch->rootIndex, relPath);
                }
            }
        }
        if (info->NextEntryOffset == 0) break;
        cursor += info->NextEntryOffset;
    }
}

static void* win32Thread(void* arg) {
    WatcherBackend* backend = arg;
    Win32State* state = backend->state;

    HANDLE handles[CONFIG_MAX_ROOTS + 1];
    handles[0] = state->stopEvent;
    for (uint32_t i = 0; i < state->rootCount; i++) handles[i + 1] = state->roots[i].overlapped.hEvent;

    while (!atomic_load(&backend->stopRequested)) {
        const DWORD result = WaitForMultipleObjects(state->rootCount + 1, handles, FALSE, 250);
        if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT || result == WAIT_FAILED) continue;
        const uint32_t index = result - WAIT_OBJECT_0 - 1;
        if (index >= state->rootCount) continue;

        RootWatch* watch = &state->roots[index];
        DWORD bytes = 0;
        if (GetOverlappedResult(watch->directory, &watch->overlapped, &bytes, FALSE)) {
            handleNotifications(backend, watch, bytes);
        }
        ResetEvent(watch->overlapped.hEvent);
        issueRead(watch);
    }
    return NULL;
}

static bool win32Start(WatcherBackend* backend) {
    Win32State* state = backend->state;
    state->stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    state->rootCount = 0;
    for (uint32_t i = 0; i < backend->config->rootCount; i++) {
        RootWatch* watch = &state->roots[state->rootCount];
        watch->directory = CreateFileA(backend->config->rootPaths[i], FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        if (watch->directory == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "watcher: cannot open '%s' for change notification\n", backend->config->rootPaths[i]);
            continue;
        }
        watch->buffer = malloc(NOTIFY_BUFFER_SIZE);
        watch->rootIndex = i;
        memset(&watch->overlapped, 0, sizeof(watch->overlapped));
        watch->overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!issueRead(watch)) {
            CloseHandle(watch->directory);
            CloseHandle(watch->overlapped.hEvent);
            free(watch->buffer);
            continue;
        }
        state->rootCount++;
    }
    printf("watcher: ReadDirectoryChangesW watching %u root(s)\n", state->rootCount);
    return platformThreadCreate(&backend->thread, win32Thread, backend);
}

static void win32Stop(WatcherBackend* backend) {
    Win32State* state = backend->state;
    SetEvent(state->stopEvent);
    platformThreadJoin(backend->thread);
    for (uint32_t i = 0; i < state->rootCount; i++) {
        CancelIoEx(state->roots[i].directory, &state->roots[i].overlapped);
        CloseHandle(state->roots[i].directory);
        CloseHandle(state->roots[i].overlapped.hEvent);
        free(state->roots[i].buffer);
    }
    CloseHandle(state->stopEvent);
    state->rootCount = 0;
}

static void win32Destroy(WatcherBackend* backend) {
    free(backend->state);
    free(backend);
}

WatcherBackend* watcherWin32Create(const ClientConfig* config, EventQueue* queue) {
    WatcherBackend* backend = calloc(1, sizeof(WatcherBackend));
    backend->name = "ReadDirectoryChangesW";
    backend->start = win32Start;
    backend->stop = win32Stop;
    backend->destroy = win32Destroy;
    backend->config = config;
    backend->queue = queue;
    backend->state = calloc(1, sizeof(Win32State));
    return backend;
}
#else
typedef int watcherWin32UnusedOnThisPlatform;
#endif
