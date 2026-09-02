//
// File-system watcher interface. The main loop only ever sees SyncEvents on the EventQueue;
// which OS facility produced them is a backend detail:
//
//   watcher_inotify.c  Linux    inotify (recursive by adding a watch per directory as it appears)
//   watcher_win32.c    Windows  ReadDirectoryChangesW (natively recursive)
//   watcher_poll.c     any      periodic stat-walk diff (macOS dev hosts, or forced via config)
//
// Every backend reports overflow (lost events) as EVENT_OVERFLOW so the main loop can fall back
// to a full scan - the design's answer to kernel event queues being finite.
//
#ifndef DATA_SYNCHRONISATION_TOOL_WATCHER_H
#define DATA_SYNCHRONISATION_TOOL_WATCHER_H
#include "config.h"
#include "eventQueue.h"

typedef struct Watcher Watcher;

Watcher* watcherCreate(const ClientConfig* config, EventQueue* queue);
bool watcherStart(Watcher* watcher);   // spawns the backend thread
void watcherStop(Watcher* watcher);    // signals and joins it
void watcherDestroy(Watcher* watcher);
const char* watcherBackendName(const Watcher* watcher);

#endif //DATA_SYNCHRONISATION_TOOL_WATCHER_H
