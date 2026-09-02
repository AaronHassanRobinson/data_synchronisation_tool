//
// Transport interface. Everything above this layer (protocol framing, sync logic) talks to a
// `Transport` and never to a socket directly, so the wire can be swapped without touching them.
//
// Shipped backends: plain TCP, and TCP+TLS via OpenSSL. The design doc calls for QUIC (MsQuic);
// that library isn't obtainable through either target platform's package manager, so it's not
// bundled - a QUIC backend would implement the same three function pointers.
//
#ifndef DATA_SYNCHRONISATION_TOOL_TRANSPORT_H
#define DATA_SYNCHRONISATION_TOOL_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include "platform.h"

typedef struct Transport Transport;
struct Transport {
    bool (*sendAll)(Transport* self, const void* data, size_t length);
    bool (*recvAll)(Transport* self, void* buffer, size_t length);
    void (*close)(Transport* self); // releases everything, including the socket, and frees `self`
    void* context;
    SocketFd fd;
};

// --- TCP plumbing ---
SocketFd tcpConnect(const char* host, uint16_t port, uint32_t timeoutMs);
SocketFd tcpListen(uint16_t port);
SocketFd tcpAccept(SocketFd listenFd);

// --- backends ---
Transport* transportPlain(SocketFd fd);

typedef struct {
    const char* certificatePath; // PEM
    const char* privateKeyPath;  // PEM
} TlsServerConfig;

typedef struct {
    // Hex SHA-256 of the server's DER certificate. Empty/NULL = accept any certificate (with a
    // warning) - fine for a first run, but pin it in production so a MITM can't stand in.
    const char* pinnedCertificateSha256;
} TlsClientConfig;

bool tlsAvailable(void);
Transport* transportTlsServer(SocketFd fd, const TlsServerConfig* config); // NULL on handshake failure
Transport* transportTlsClient(SocketFd fd, const TlsClientConfig* config);

static inline bool transportSend(Transport* t, const void* data, size_t length) { return t->sendAll(t, data, length); }
static inline bool transportRecv(Transport* t, void* buffer, size_t length) { return t->recvAll(t, buffer, length); }
static inline void transportClose(Transport* t) { if (t) t->close(t); }

#endif //DATA_SYNCHRONISATION_TOOL_TRANSPORT_H
