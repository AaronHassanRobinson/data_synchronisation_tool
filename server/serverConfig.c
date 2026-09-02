#include "serverConfig.h"
#include "jsonUtil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void serverConfigDefaults(ServerConfig* c) {
    memset(c, 0, sizeof(*c));
    c->listenPort = 9001;
    snprintf(c->outputDirectory, sizeof(c->outputDirectory), "server_data");
    c->useTls = true;
    snprintf(c->tlsCertificatePath, sizeof(c->tlsCertificatePath), "server/certs/server.pem");
    snprintf(c->tlsPrivateKeyPath, sizeof(c->tlsPrivateKeyPath), "server/certs/server.key");
    c->socketTimeoutMs = 30000;
}

bool serverConfigParse(const char* json, size_t length, ServerConfig* c, char* error, size_t errorCapacity) {
    serverConfigDefaults(c);
    sj_Reader reader = sj_reader((char*)json, length);
    const sj_Value root = sj_read(&reader);
    if (root.type != SJ_OBJECT) { snprintf(error, errorCapacity, "config root is not a JSON object"); return false; }

    sj_Value key, value;
    uint32_t number;
    while (sj_iter_object(&reader, root, &key, &value)) {
        bool ok = true;
        if (jsonKeyIs(key, "listen_port"))              { ok = jsonToUint32(value, &number) && number <= 65535; c->listenPort = (uint16_t)number; }
        else if (jsonKeyIs(key, "output_directory"))    ok = jsonCopyString(value, c->outputDirectory, sizeof(c->outputDirectory));
        else if (jsonKeyIs(key, "pre_shared_key"))      ok = jsonCopyString(value, c->preSharedKey, sizeof(c->preSharedKey));
        else if (jsonKeyIs(key, "use_tls"))             ok = jsonToBool(value, &c->useTls);
        else if (jsonKeyIs(key, "tls_certificate"))     ok = jsonCopyString(value, c->tlsCertificatePath, sizeof(c->tlsCertificatePath));
        else if (jsonKeyIs(key, "tls_private_key"))     ok = jsonCopyString(value, c->tlsPrivateKeyPath, sizeof(c->tlsPrivateKeyPath));
        else if (jsonKeyIs(key, "socket_timeout_ms"))   ok = jsonToUint32(value, &c->socketTimeoutMs);
        else if (jsonKeyIs(key, "allowed_client_ids")) {
            if (value.type != SJ_ARRAY) ok = false;
            sj_Value item;
            while (ok && sj_iter_array(&reader, value, &item)) {
                if (c->allowedClientCount >= SERVER_MAX_ALLOWED_CLIENTS) { ok = false; break; }
                ok = jsonCopyString(item, c->allowedClientIds[c->allowedClientCount++], 65);
            }
        }
        if (!ok) {
            snprintf(error, errorCapacity, "invalid value for '%.*s'", (int)(key.end - key.start), key.start);
            return false;
        }
    }
    if (reader.error) { snprintf(error, errorCapacity, "json: %s", reader.error); return false; }
    if (c->preSharedKey[0] == '\0') { snprintf(error, errorCapacity, "pre_shared_key is required"); return false; }
    return true;
}

bool serverConfigLoad(const char* path, ServerConfig* config, char* error, size_t errorCapacity) {
    uint8_t* data = NULL;
    size_t length = 0;
    if (!readFileBytes(path, &data, &length)) {
        snprintf(error, errorCapacity, "could not read config file '%s'", path);
        return false;
    }
    const bool ok = serverConfigParse((const char*)data, length, config, error, errorCapacity);
    free(data);
    return ok;
}

bool serverConfigClientAllowed(const ServerConfig* config, const char* clientId) {
    if (config->allowedClientCount == 0) return true;
    for (uint32_t i = 0; i < config->allowedClientCount; i++) {
        if (strcmp(config->allowedClientIds[i], clientId) == 0) return true;
    }
    return false;
}
