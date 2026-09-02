//
// A connected, authenticated session: TCP (+TLS) connect, HELLO version/capability
// negotiation, mutual PSK challenge/response, and announcing the watched roots.
//
#ifndef DATA_SYNCHRONISATION_TOOL_SESSION_H
#define DATA_SYNCHRONISATION_TOOL_SESSION_H
#include "config.h"
#include "protocol.h"

typedef struct {
    ProtocolLink link;
    uint16_t version;
    bool connected;
} Session;

bool sessionConnect(Session* session, const ClientConfig* config);
void sessionClose(Session* session); // sends BYE if still connected

// Label the server sees for a root: the last path component (with a fallback for "/" or "C:\").
void sessionRootLabel(const char* rootPath, char* out, size_t capacity);

#endif //DATA_SYNCHRONISATION_TOOL_SESSION_H
