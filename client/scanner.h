//
// The periodic full recursive scan. Walks every root and compares each file's stat triple with
// the DB; anything new or changed is queued as if the watcher had reported it. This is the
// backstop for watcher overflow, for directories created before their watch existed, for
// tainted reads that were skipped, and for anything that changed while the process was down.
//
#ifndef DATA_SYNCHRONISATION_TOOL_SCANNER_H
#define DATA_SYNCHRONISATION_TOOL_SCANNER_H
#include "config.h"
#include "db.h"
#include "eventQueue.h"

typedef struct {
    uint32_t filesSeen;
    uint32_t filesQueued;
    uint32_t directoriesSeen;
    uint32_t directoriesQueued;
} ScanStats;

ScanStats scannerFullScan(const ClientConfig* config, const StateDb* db, EventQueue* queue);

#endif //DATA_SYNCHRONISATION_TOOL_SCANNER_H
