//
// One client connection, start to finish: handshake, auth, root registration, then the
// DIR_META / FILE_MANIFEST / CHUNK_DATA / FILE_FINISH loop until BYE or disconnect.
//
#ifndef DATA_SYNCHRONISATION_TOOL_SERVER_SESSION_H
#define DATA_SYNCHRONISATION_TOOL_SERVER_SESSION_H
#include "serverConfig.h"
#include "chunkStore.h"
#include "transport.h"

typedef struct {
    uint32_t filesReceived;
    uint32_t filesFailed;
    uint32_t chunksReceived;
    uint32_t chunksRejected;
    uint64_t bytesReceived;
    uint64_t bytesReused;
} ServerSessionStats;

// Runs the whole session on an already-accepted socket; closes it before returning.
ServerSessionStats serverSessionRun(SocketFd fd, const ServerConfig* config, ChunkStore* store);

// Turns a client-supplied root label into a directory name that can't escape the output
// directory (only [A-Za-z0-9._-] survive; empty/dot-only labels become "root").
void serverSanitizeLabel(const char* label, char* out, size_t capacity);

#endif //DATA_SYNCHRONISATION_TOOL_SERVER_SESSION_H
