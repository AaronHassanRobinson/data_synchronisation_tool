//
// Created by MacbookPro on 22/8/2026.
//
#include <protocol.h>
#include <sys/socket.h>
#include <stdlib.h>

bool sendAll(const int socketFd, const void* data, const size_t length) {
    const uint8_t* bytes = data;
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(socketFd, bytes + sent, length - sent, 0);
        if (result <= 0) return false; // error, or peer gone
        sent += (size_t)result;
    }
    return true;
}

bool recvAll(const int socketFd, void* buffer, const size_t length) {
    uint8_t* bytes = buffer;
    size_t received = 0;
    while (received < length) {
        const ssize_t result = recv(socketFd, bytes + received, length - received, 0);
        if (result <= 0) return false; // 0 = peer closed the connection, <0 = error
        received += (size_t)result;
    }
    return true;
}

bool sendMessage(const int socketFd, const MessageType type, const void* payload, const uint32_t payloadLength) {
    const ProtocolHeader header = {.type = type, .length = payloadLength};
    if (!sendAll(socketFd, &header, sizeof(header))) return false;
    if (payloadLength > 0 && !sendAll(socketFd, payload, payloadLength)) return false;
    return true;
}

bool recvMessage(const int socketFd, MessageType* outType, void** outPayload, uint32_t* outPayloadLength) {
    ProtocolHeader header;
    if (!recvAll(socketFd, &header, sizeof(header))) return false;

    void* payload = NULL;
    if (header.length > 0) {
        payload = malloc(header.length);
        if (payload == NULL) return false;
        if (!recvAll(socketFd, payload, header.length)) {
            free(payload);
            return false;
        }
    }

    *outType = header.type;
    *outPayload = payload;
    *outPayloadLength = header.length;
    return true;
}
