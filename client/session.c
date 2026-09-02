#include "session.h"
#include "cdcProtocol.h"
#include "compress.h"
#include "hmac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sessionRootLabel(const char* rootPath, char* out, size_t capacity) {
    char trimmed[SYNC_PATH_MAX];
    snprintf(trimmed, sizeof(trimmed), "%s", rootPath);
    size_t length = strlen(trimmed);
    while (length > 1 && (trimmed[length - 1] == '/' || trimmed[length - 1] == '\\')) trimmed[--length] = '\0';

    const char* slash = strrchr(trimmed, '/');
    const char* backslash = strrchr(trimmed, '\\');
    const char* last = slash > backslash ? slash : backslash;
    const char* base = last ? last + 1 : trimmed;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) base = "root";
    snprintf(out, capacity, "%s", base);
}

static bool negotiate(Session* session, const ClientConfig* config, uint8_t serverNonce[PROTOCOL_NONCE_SIZE]) {
    const uint32_t capabilities = (config->useCompression && compressionAvailable()) ? CAP_ZSTD : 0;
    uint32_t length = 0;
    void* hello = buildHelloPayload(config->clientId, capabilities, &config->cdc, &length);
    const bool sent = sendMessage(&session->link, MSG_HELLO, hello, length);
    free(hello);
    if (!sent) return false;

    void* payload = NULL;
    if (!expectMessage(&session->link, MSG_HELLO_ACK, sizeof(HelloAckHeader), &payload, &length)) return false;
    HelloAckHeader ack;
    memcpy(&ack, payload, sizeof(ack));
    free(payload);

    if (ack.chosenVersion < PROTOCOL_VERSION_MIN || ack.chosenVersion > PROTOCOL_VERSION_MAX) {
        fprintf(stderr, "session: server rejected our protocol versions %u-%u\n", PROTOCOL_VERSION_MIN, PROTOCOL_VERSION_MAX);
        return false;
    }
    session->version = ack.chosenVersion;
    session->link.compressionEnabled = (ack.capabilities & capabilities & CAP_ZSTD) != 0;
    memcpy(serverNonce, ack.nonce, PROTOCOL_NONCE_SIZE);
    return true;
}

static bool authenticate(Session* session, const ClientConfig* config, const uint8_t serverNonce[PROTOCOL_NONCE_SIZE]) {
    // Client proves it knows the PSK: HMAC(psk, serverNonce || clientId)
    uint8_t message[PROTOCOL_NONCE_SIZE + PROTOCOL_MAX_CLIENT_ID];
    memcpy(message, serverNonce, PROTOCOL_NONCE_SIZE);
    const size_t idLength = strlen(config->clientId);
    memcpy(message + PROTOCOL_NONCE_SIZE, config->clientId, idLength);

    AuthHeader auth;
    hmacSha256((const uint8_t*)config->preSharedKey, strlen(config->preSharedKey), message, PROTOCOL_NONCE_SIZE + idLength, auth.mac);
    if (!randomBytes(auth.clientNonce, PROTOCOL_NONCE_SIZE)) return false;
    if (!sendMessage(&session->link, MSG_AUTH, &auth, sizeof(auth))) return false;

    void* payload = NULL;
    uint32_t length = 0;
    if (!expectMessage(&session->link, MSG_AUTH_ACK, sizeof(AuthAckHeader), &payload, &length)) return false;
    AuthAckHeader ack;
    memcpy(&ack, payload, sizeof(ack));
    free(payload);
    if (!ack.success) {
        fprintf(stderr, "session: server rejected our credentials\n");
        return false;
    }

    // Server proves it knows the PSK too: HMAC(psk, clientNonce || "server")
    uint8_t expectedMessage[PROTOCOL_NONCE_SIZE + 6];
    memcpy(expectedMessage, auth.clientNonce, PROTOCOL_NONCE_SIZE);
    memcpy(expectedMessage + PROTOCOL_NONCE_SIZE, "server", 6);
    uint8_t expected[SHA256_DIGEST_SIZE];
    hmacSha256((const uint8_t*)config->preSharedKey, strlen(config->preSharedKey), expectedMessage, sizeof(expectedMessage), expected);
    if (!constantTimeEquals(expected, ack.mac, SHA256_DIGEST_SIZE)) {
        fprintf(stderr, "session: server failed to prove it knows the pre-shared key\n");
        return false;
    }
    return true;
}

static bool announceRoots(Session* session, const ClientConfig* config) {
    char labels[CONFIG_MAX_ROOTS][256];
    const char* labelPointers[CONFIG_MAX_ROOTS];
    for (uint32_t i = 0; i < config->rootCount; i++) {
        sessionRootLabel(config->rootPaths[i], labels[i], sizeof(labels[i]));
        labelPointers[i] = labels[i];
    }
    uint32_t length = 0;
    void* payload = buildWatchRootsPayload(labelPointers, config->rootCount, &length);
    const bool sent = sendMessage(&session->link, MSG_WATCH_ROOTS, payload, length);
    free(payload);
    if (!sent) return false;

    void* ackPayload = NULL;
    if (!expectMessage(&session->link, MSG_WATCH_ROOTS_ACK, sizeof(AckHeader), &ackPayload, &length)) return false;
    const bool ok = ((AckHeader*)ackPayload)->ok != 0;
    free(ackPayload);
    if (!ok) fprintf(stderr, "session: server refused the watched root list\n");
    return ok;
}

bool sessionConnect(Session* session, const ClientConfig* config) {
    memset(session, 0, sizeof(*session));

    if (config->useTls && !tlsAvailable()) {
        fprintf(stderr, "session: use_tls is set but this build has no OpenSSL - refusing to connect in plaintext\n");
        return false;
    }

    const SocketFd fd = tcpConnect(config->serverIp, config->serverPort, config->socketTimeoutMs);
    if (fd == INVALID_SOCKET_FD) return false;

    Transport* transport;
    if (config->useTls) {
        const TlsClientConfig tls = { .pinnedCertificateSha256 = config->serverCertificateSha256 };
        transport = transportTlsClient(fd, &tls);
        if (!transport) { platformCloseSocket(fd); return false; }
    } else {
        transport = transportPlain(fd);
    }
    session->link.transport = transport;

    uint8_t serverNonce[PROTOCOL_NONCE_SIZE];
    if (!negotiate(session, config, serverNonce) || !authenticate(session, config, serverNonce) || !announceRoots(session, config)) {
        transportClose(transport);
        session->link.transport = NULL;
        return false;
    }
    session->connected = true;
    printf("session: connected to %s:%u (protocol v%u, %s, compression %s)\n", config->serverIp, config->serverPort,
           session->version, config->useTls ? "TLS" : "PLAINTEXT", session->link.compressionEnabled ? "on" : "off");
    return true;
}

void sessionClose(Session* session) {
    if (session->link.transport) {
        if (session->connected) sendMessage(&session->link, MSG_BYE, NULL, 0);
        transportClose(session->link.transport);
        session->link.transport = NULL;
    }
    session->connected = false;
}
