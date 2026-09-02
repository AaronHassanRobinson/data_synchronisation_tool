//
// Collection server: accepts one client at a time (the design assumes a 1:1 client/server
// pairing), runs its session, then goes back to accepting - so a client that lost its
// connection can simply reconnect. Files are only ever added or replaced with verified content;
// nothing the client deletes is ever deleted here.
//
//   sync_server [serverConfig.json]
//
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include "serverConfig.h"
#include "serverSession.h"

#define DEFAULT_SERVER_CONFIG "server/serverConfig.json"

static atomic_bool stopRequested = false;
static SocketFd listenFd = INVALID_SOCKET_FD;
static void onSignal(int signal) {
    (void)signal;
    atomic_store(&stopRequested, true);
    platformCloseSocket(listenFd); // unblocks accept()
}

int main(int argc, char** argv) {
    const char* configPath = argc > 1 ? argv[1] : DEFAULT_SERVER_CONFIG;
    setvbuf(stdout, NULL, _IOLBF, 0); // line-buffered even when logging to a file
    printf("Data synchronisation tool - server\n");

    ServerConfig config;
    char error[256];
    if (!serverConfigLoad(configPath, &config, error, sizeof(error))) {
        fprintf(stderr, "config: %s\n", error);
        return 1;
    }
    if (config.useTls && !tlsAvailable()) {
        fprintf(stderr, "config: use_tls is set but this build has no OpenSSL - refusing to serve in plaintext\n");
        return 1;
    }
    if (!platformNetInit() || !mkdirRecursive(config.outputDirectory)) return 1;

    char storePath[SYNC_PATH_MAX];
    pathJoin(storePath, sizeof(storePath), config.outputDirectory, ".chunks");
    ChunkStore* store = chunkStoreOpen(storePath);
    if (!store) { fprintf(stderr, "could not create chunk store at %s\n", storePath); return 1; }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // a peer that vanished mid-write is a return code, not a death sentence
#endif
    listenFd = tcpListen(config.listenPort);
    if (listenFd == INVALID_SOCKET_FD) {
        fprintf(stderr, "could not listen on port %u\n", config.listenPort);
        return 1;
    }
    printf("listening on port %u, %s, writing to '%s'\n", config.listenPort, config.useTls ? "TLS" : "PLAINTEXT", config.outputDirectory);

    while (!atomic_load(&stopRequested)) {
        const SocketFd client = tcpAccept(listenFd);
        if (client == INVALID_SOCKET_FD) continue;
        printf("session: connection accepted\n");
        serverSessionRun(client, &config, store);
    }

    chunkStoreClose(store);
    platformNetShutdown();
    printf("server: stopped\n");
    return 0;
}
