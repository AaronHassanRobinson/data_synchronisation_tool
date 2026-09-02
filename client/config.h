//
// clientConfig.json. Human-readable, editable on the fly; loaded once at startup (a daemon
// restart - which the cron/systemd/launchd/schtasks wrappers in deploy/ guarantee - picks up
// changes). Every tunable the design calls "configurable" lives here.
//
#ifndef DATA_SYNCHRONISATION_TOOL_CONFIG_H
#define DATA_SYNCHRONISATION_TOOL_CONFIG_H
#include <stdint.h>
#include "cdc.h"
#include "fileUtil.h"

#define CONFIG_MAX_ROOTS 32

typedef enum {
    WATCHER_BACKEND_NATIVE = 0, // inotify on Linux, ReadDirectoryChangesW on Windows, polling elsewhere
    WATCHER_BACKEND_POLL = 1,   // force the portable polling backend
} WatcherBackendChoice;

typedef struct {
    // --- server / identity ---
    char serverIp[64];
    uint16_t serverPort;
    char clientId[PROTOCOL_MAX_CLIENT_ID + 1];
    char preSharedKey[256];
    char serverCertificateSha256[65]; // optional TLS pin
    bool useTls;
    bool useCompression;

    // --- what to sync ---
    char rootPaths[CONFIG_MAX_ROOTS][SYNC_PATH_MAX];
    uint32_t rootCount;
    char databasePath[SYNC_PATH_MAX];

    // --- change detection ---
    WatcherBackendChoice watcherBackend;
    uint32_t scanIntervalSeconds;   // periodic full recursive scan
    uint32_t batchMaxEvents;        // flush a batch once this many distinct events are pending...
    uint32_t batchWindowSeconds;    // ...or once this long has passed since the last event
    uint32_t pollIntervalSeconds;   // polling watcher cadence

    // --- networking ---
    uint32_t reconnectDelaySeconds;
    uint32_t socketTimeoutMs;
    uint32_t chunkRetryLimit;

    // --- chunking ---
    CdcParams cdc;
    char hashAlgorithm[16];         // only "sha256" is implemented; validated so a typo fails loudly
} ClientConfig;

void clientConfigDefaults(ClientConfig* config);
bool clientConfigLoad(const char* path, ClientConfig* config, char* error, size_t errorCapacity);
// Parses from an in-memory JSON string (what clientConfigLoad calls; exposed for unit tests).
bool clientConfigParse(const char* json, size_t length, ClientConfig* config, char* error, size_t errorCapacity);
void clientConfigPrint(const ClientConfig* config);

#endif //DATA_SYNCHRONISATION_TOOL_CONFIG_H
