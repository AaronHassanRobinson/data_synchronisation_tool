#include "transport.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#endif

// ---------------------------------------------------------------- TCP plumbing

SocketFd tcpConnect(const char* host, uint16_t port, uint32_t timeoutMs) {
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) <= 0) return INVALID_SOCKET_FD;

    const SocketFd fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_FD) return INVALID_SOCKET_FD;

    platformSetSocketTimeout(fd, timeoutMs);
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        platformCloseSocket(fd);
        return INVALID_SOCKET_FD;
    }
    return fd;
}

SocketFd tcpListen(uint16_t port) {
    const SocketFd fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_FD) return INVALID_SOCKET_FD;

    const int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(enable));

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(fd, 1) != 0) {
        platformCloseSocket(fd);
        return INVALID_SOCKET_FD;
    }
    return fd;
}

SocketFd tcpAccept(SocketFd listenFd) {
    struct sockaddr_in address;
    socklen_t addressLength = sizeof(address);
    return accept(listenFd, (struct sockaddr*)&address, &addressLength);
}

// ---------------------------------------------------------------- plain backend

static bool plainSendAll(Transport* self, const void* data, size_t length) {
    const uint8_t* bytes = data;
    size_t sent = 0;
    while (sent < length) {
        const long result = (long)send(self->fd, (const char*)bytes + sent, (int)(length - sent), 0);
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static bool plainRecvAll(Transport* self, void* buffer, size_t length) {
    uint8_t* bytes = buffer;
    size_t received = 0;
    while (received < length) {
        const long result = (long)recv(self->fd, (char*)bytes + received, (int)(length - received), 0);
        if (result <= 0) return false; // 0 = peer closed, <0 = error/timeout
        received += (size_t)result;
    }
    return true;
}

static void plainClose(Transport* self) {
    platformCloseSocket(self->fd);
    free(self);
}

Transport* transportPlain(SocketFd fd) {
    Transport* t = calloc(1, sizeof(Transport));
    t->sendAll = plainSendAll;
    t->recvAll = plainRecvAll;
    t->close = plainClose;
    t->fd = fd;
    return t;
}

// ---------------------------------------------------------------- TLS backend

#ifdef SYNC_HAVE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

typedef struct {
    SSL_CTX* ctx;
    SSL* ssl;
} TlsContext;

bool tlsAvailable(void) { return true; }

static bool tlsSendAll(Transport* self, const void* data, size_t length) {
    const TlsContext* tls = self->context;
    const uint8_t* bytes = data;
    size_t sent = 0;
    while (sent < length) {
        const int result = SSL_write(tls->ssl, bytes + sent, (int)(length - sent));
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static bool tlsRecvAll(Transport* self, void* buffer, size_t length) {
    const TlsContext* tls = self->context;
    uint8_t* bytes = buffer;
    size_t received = 0;
    while (received < length) {
        const int result = SSL_read(tls->ssl, bytes + received, (int)(length - received));
        if (result <= 0) return false;
        received += (size_t)result;
    }
    return true;
}

static void tlsClose(Transport* self) {
    TlsContext* tls = self->context;
    if (tls->ssl) { SSL_shutdown(tls->ssl); SSL_free(tls->ssl); }
    if (tls->ctx) SSL_CTX_free(tls->ctx);
    free(tls);
    platformCloseSocket(self->fd);
    free(self);
}

static Transport* tlsWrap(SocketFd fd, SSL_CTX* ctx, SSL* ssl) {
    TlsContext* tls = calloc(1, sizeof(TlsContext));
    tls->ctx = ctx;
    tls->ssl = ssl;
    Transport* t = calloc(1, sizeof(Transport));
    t->sendAll = tlsSendAll;
    t->recvAll = tlsRecvAll;
    t->close = tlsClose;
    t->context = tls;
    t->fd = fd;
    return t;
}

static void tlsLogErrors(const char* what) {
    fprintf(stderr, "tls: %s failed: %s\n", what, ERR_error_string(ERR_get_error(), NULL));
}

Transport* transportTlsServer(SocketFd fd, const TlsServerConfig* config) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { tlsLogErrors("SSL_CTX_new"); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, config->certificatePath, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, config->privateKeyPath, SSL_FILETYPE_PEM) != 1) {
        tlsLogErrors("loading certificate/key");
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, (int)fd);
    if (SSL_accept(ssl) != 1) {
        tlsLogErrors("SSL_accept");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return tlsWrap(fd, ctx, ssl);
}

static bool tlsCertificateMatchesPin(SSL* ssl, const char* pinnedHex) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509* cert = SSL_get1_peer_certificate(ssl);
#else
    X509* cert = SSL_get_peer_certificate(ssl);
#endif
    if (!cert) return false;
    unsigned char* der = NULL;
    const int derLength = i2d_X509(cert, &der);
    X509_free(cert);
    if (derLength <= 0) return false;

    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256Buffer(der, (size_t)derLength, digest);
    OPENSSL_free(der);

    char hex[65];
    sha256ToHex(digest, hex);
    bool matches = strlen(pinnedHex) == 64;
    for (int i = 0; matches && i < 64; i++) {
        const char a = hex[i], b = pinnedHex[i];
        matches = a == b || (b >= 'A' && b <= 'F' && a == b + ('a' - 'A'));
    }
    if (!matches) {
        fprintf(stderr, "tls: server certificate fingerprint %s does not match pinned %s\n", hex, pinnedHex);
        return false;
    }
    return true;
}

Transport* transportTlsClient(SocketFd fd, const TlsClientConfig* config) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { tlsLogErrors("SSL_CTX_new"); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // Trust is established by certificate pinning below rather than a CA chain: a 1:1
    // client/server pair with a self-signed server certificate is the expected deployment.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, (int)fd);
    if (SSL_connect(ssl) != 1) {
        tlsLogErrors("SSL_connect");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (config->pinnedCertificateSha256 && config->pinnedCertificateSha256[0] != '\0') {
        if (!tlsCertificateMatchesPin(ssl, config->pinnedCertificateSha256)) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return NULL;
        }
    } else {
        fprintf(stderr, "tls: WARNING no server certificate pin configured - connection is encrypted but the server is unauthenticated\n");
    }
    return tlsWrap(fd, ctx, ssl);
}

#else

bool tlsAvailable(void) { return false; }
Transport* transportTlsServer(SocketFd fd, const TlsServerConfig* config) { (void)fd; (void)config; return NULL; }
Transport* transportTlsClient(SocketFd fd, const TlsClientConfig* config) { (void)fd; (void)config; return NULL; }

#endif
