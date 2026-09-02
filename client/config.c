#include "config.h"
#include "jsonUtil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void clientConfigDefaults(ClientConfig* c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->serverIp, sizeof(c->serverIp), "127.0.0.1");
    c->serverPort = 9001;
    snprintf(c->clientId, sizeof(c->clientId), "client-1");
    c->useTls = true;
    c->useCompression = true;
    snprintf(c->databasePath, sizeof(c->databasePath), "sync-state.db");
    c->watcherBackend = WATCHER_BACKEND_NATIVE;
    c->scanIntervalSeconds = 900;
    c->batchMaxEvents = 100;
    c->batchWindowSeconds = 5;
    c->pollIntervalSeconds = 2;
    c->reconnectDelaySeconds = 5;
    c->socketTimeoutMs = 30000;
    c->chunkRetryLimit = 3;
    c->cdc = (CdcParams){ CDC_DEFAULT_MIN_CHUNK_SIZE, CDC_DEFAULT_MAX_CHUNK_SIZE, CDC_DEFAULT_MASK_BITS };
    snprintf(c->hashAlgorithm, sizeof(c->hashAlgorithm), "sha256");
}

static bool fail(char* error, size_t capacity, const char* message) {
    snprintf(error, capacity, "%s", message);
    return false;
}

static bool addRoot(ClientConfig* c, sj_Value value, char* error, size_t errorCapacity) {
    if (c->rootCount >= CONFIG_MAX_ROOTS) return fail(error, errorCapacity, "too many directory_paths");
    if (!jsonCopyString(value, c->rootPaths[c->rootCount], SYNC_PATH_MAX)) return fail(error, errorCapacity, "directory path too long");
    // Strip a trailing separator so relative paths join cleanly.
    char* path = c->rootPaths[c->rootCount];
    size_t length = strlen(path);
    while (length > 1 && (path[length - 1] == '/' || path[length - 1] == '\\')) path[--length] = '\0';
    c->rootCount++;
    return true;
}

bool clientConfigParse(const char* json, size_t length, ClientConfig* c, char* error, size_t errorCapacity) {
    clientConfigDefaults(c);
    sj_Reader reader = sj_reader((char*)json, length);
    const sj_Value root = sj_read(&reader);
    if (root.type != SJ_OBJECT) return fail(error, errorCapacity, "config root is not a JSON object");

    sj_Value key, value;
    uint32_t number;
    while (sj_iter_object(&reader, root, &key, &value)) {
        bool ok = true;
        if (jsonKeyIs(key, "server_ip_address"))            ok = jsonCopyString(value, c->serverIp, sizeof(c->serverIp));
        else if (jsonKeyIs(key, "server_port"))             { ok = jsonToUint32(value, &number) && number <= 65535; c->serverPort = (uint16_t)number; }
        else if (jsonKeyIs(key, "client_id"))               ok = jsonCopyString(value, c->clientId, sizeof(c->clientId));
        else if (jsonKeyIs(key, "pre_shared_key"))          ok = jsonCopyString(value, c->preSharedKey, sizeof(c->preSharedKey));
        else if (jsonKeyIs(key, "server_certificate_sha256")) ok = jsonCopyString(value, c->serverCertificateSha256, sizeof(c->serverCertificateSha256));
        else if (jsonKeyIs(key, "use_tls"))                 ok = jsonToBool(value, &c->useTls);
        else if (jsonKeyIs(key, "use_compression"))         ok = jsonToBool(value, &c->useCompression);
        else if (jsonKeyIs(key, "database_path"))           ok = jsonCopyString(value, c->databasePath, sizeof(c->databasePath));
        else if (jsonKeyIs(key, "scan_interval_seconds"))   ok = jsonToUint32(value, &c->scanIntervalSeconds);
        else if (jsonKeyIs(key, "batch_max_events"))        ok = jsonToUint32(value, &c->batchMaxEvents);
        else if (jsonKeyIs(key, "batch_window_seconds"))    ok = jsonToUint32(value, &c->batchWindowSeconds);
        else if (jsonKeyIs(key, "poll_interval_seconds"))   ok = jsonToUint32(value, &c->pollIntervalSeconds);
        else if (jsonKeyIs(key, "reconnect_delay_seconds")) ok = jsonToUint32(value, &c->reconnectDelaySeconds);
        else if (jsonKeyIs(key, "socket_timeout_ms"))       ok = jsonToUint32(value, &c->socketTimeoutMs);
        else if (jsonKeyIs(key, "chunk_retry_limit"))       ok = jsonToUint32(value, &c->chunkRetryLimit);
        else if (jsonKeyIs(key, "cdc_min_chunk_size"))      ok = jsonToUint32(value, &c->cdc.minChunkSize);
        else if (jsonKeyIs(key, "cdc_max_chunk_size"))      ok = jsonToUint32(value, &c->cdc.maxChunkSize);
        else if (jsonKeyIs(key, "cdc_mask_bits"))           ok = jsonToUint32(value, &c->cdc.maskBits);
        else if (jsonKeyIs(key, "hash_algorithm"))          ok = jsonCopyString(value, c->hashAlgorithm, sizeof(c->hashAlgorithm));
        else if (jsonKeyIs(key, "watcher_backend")) {
            char name[16];
            ok = jsonCopyString(value, name, sizeof(name));
            if (ok && strcmp(name, "poll") == 0) c->watcherBackend = WATCHER_BACKEND_POLL;
            else if (ok && strcmp(name, "native") == 0) c->watcherBackend = WATCHER_BACKEND_NATIVE;
            else ok = false;
        }
        else if (jsonKeyIs(key, "directory_paths")) {
            if (value.type == SJ_ARRAY) {
                sj_Value item;
                while (sj_iter_array(&reader, value, &item)) {
                    if (!addRoot(c, item, error, errorCapacity)) return false;
                }
            } else if (value.type == SJ_STRING) {
                if (!addRoot(c, value, error, errorCapacity)) return false;
            } else ok = false;
        }
        // Unknown keys are ignored so a newer config still loads on an older client.

        if (!ok) {
            snprintf(error, errorCapacity, "invalid value for '%.*s'", (int)(key.end - key.start), key.start);
            return false;
        }
    }
    if (reader.error) { snprintf(error, errorCapacity, "json: %s", reader.error); return false; }

    if (c->rootCount == 0) return fail(error, errorCapacity, "directory_paths must list at least one directory");
    if (c->preSharedKey[0] == '\0') return fail(error, errorCapacity, "pre_shared_key is required");
    if (!cdcParamsValid(&c->cdc)) return fail(error, errorCapacity, "cdc_* chunk parameters are out of range");
    if (strcmp(c->hashAlgorithm, "sha256") != 0) return fail(error, errorCapacity, "hash_algorithm: only \"sha256\" is implemented");
    if (c->batchMaxEvents == 0) c->batchMaxEvents = 1;
    return true;
}

bool clientConfigLoad(const char* path, ClientConfig* config, char* error, size_t errorCapacity) {
    uint8_t* data = NULL;
    size_t length = 0;
    if (!readFileBytes(path, &data, &length)) {
        snprintf(error, errorCapacity, "could not read config file '%s'", path);
        return false;
    }
    const bool ok = clientConfigParse((const char*)data, length, config, error, errorCapacity);
    free(data);
    return ok;
}

void clientConfigPrint(const ClientConfig* c) {
    printf("config: server=%s:%u client_id=%s tls=%s compression=%s\n", c->serverIp, c->serverPort, c->clientId,
           c->useTls ? "on" : "off", c->useCompression ? "on" : "off");
    printf("config: db=%s scan_interval=%us batch=%u events / %us window watcher=%s\n", c->databasePath,
           c->scanIntervalSeconds, c->batchMaxEvents, c->batchWindowSeconds,
           c->watcherBackend == WATCHER_BACKEND_POLL ? "poll" : "native");
    printf("config: cdc min=%u max=%u mask_bits=%u hash=%s\n", c->cdc.minChunkSize, c->cdc.maxChunkSize, c->cdc.maskBits, c->hashAlgorithm);
    for (uint32_t i = 0; i < c->rootCount; i++) printf("config: root[%u]=%s\n", i, c->rootPaths[i]);
}
