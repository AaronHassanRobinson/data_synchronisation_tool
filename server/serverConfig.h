#ifndef DATA_SYNCHRONISATION_TOOL_SERVER_CONFIG_H
#define DATA_SYNCHRONISATION_TOOL_SERVER_CONFIG_H
#include <stdint.h>
#include "fileUtil.h"

#define SERVER_MAX_ALLOWED_CLIENTS 16

typedef struct {
    uint16_t listenPort;
    char outputDirectory[SYNC_PATH_MAX];
    char preSharedKey[256];
    bool useTls;
    char tlsCertificatePath[SYNC_PATH_MAX];
    char tlsPrivateKeyPath[SYNC_PATH_MAX];
    char allowedClientIds[SERVER_MAX_ALLOWED_CLIENTS][65];
    uint32_t allowedClientCount; // 0 = any client id (the PSK still gates access)
    uint32_t socketTimeoutMs;
} ServerConfig;

void serverConfigDefaults(ServerConfig* config);
bool serverConfigParse(const char* json, size_t length, ServerConfig* config, char* error, size_t errorCapacity);
bool serverConfigLoad(const char* path, ServerConfig* config, char* error, size_t errorCapacity);
bool serverConfigClientAllowed(const ServerConfig* config, const char* clientId);

#endif //DATA_SYNCHRONISATION_TOOL_SERVER_CONFIG_H
