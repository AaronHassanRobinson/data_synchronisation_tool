#include "protocol.h"
#include "compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Payloads smaller than this aren't worth the zstd framing overhead.
#define COMPRESSION_THRESHOLD 128

bool sendMessage(ProtocolLink* link, MessageType type, const void* payload, uint32_t payloadLength) {
    ProtocolHeader header = {
        .magic = PROTOCOL_MAGIC,
        .type = (uint16_t)type,
        .flags = 0,
        .rawLength = payloadLength,
        .wireLength = payloadLength,
    };

    uint8_t* compressed = NULL;
    const void* wirePayload = payload;
    if (link->compressionEnabled && payloadLength >= COMPRESSION_THRESHOLD) {
        size_t compressedLength = 0;
        compressed = compressBuffer(payload, payloadLength, &compressedLength);
        if (compressed && compressedLength < payloadLength) {
            header.flags |= MSG_FLAG_COMPRESSED;
            header.wireLength = (uint32_t)compressedLength;
            wirePayload = compressed;
        }
    }

    bool ok = transportSend(link->transport, &header, sizeof(header));
    if (ok && header.wireLength > 0) ok = transportSend(link->transport, wirePayload, header.wireLength);
    free(compressed);
    return ok;
}

bool recvMessage(ProtocolLink* link, MessageType* outType, void** outPayload, uint32_t* outPayloadLength) {
    ProtocolHeader header;
    if (!transportRecv(link->transport, &header, sizeof(header))) return false;
    if (header.magic != PROTOCOL_MAGIC) {
        fprintf(stderr, "protocol: bad magic 0x%08x, dropping connection\n", header.magic);
        return false;
    }
    if (header.rawLength > PROTOCOL_MAX_PAYLOAD || header.wireLength > PROTOCOL_MAX_PAYLOAD) {
        fprintf(stderr, "protocol: oversized payload (%u/%u), dropping connection\n", header.rawLength, header.wireLength);
        return false;
    }

    void* payload = NULL;
    if (header.wireLength > 0) {
        payload = malloc(header.wireLength);
        if (!payload) return false;
        if (!transportRecv(link->transport, payload, header.wireLength)) { free(payload); return false; }
    }

    if (header.flags & MSG_FLAG_COMPRESSED) {
        uint8_t* raw = decompressBuffer(payload, header.wireLength, header.rawLength);
        free(payload);
        if (!raw) {
            fprintf(stderr, "protocol: failed to decompress %s payload\n", messageTypeName((MessageType)header.type));
            return false;
        }
        payload = raw;
    } else if (header.rawLength != header.wireLength) {
        free(payload);
        return false;
    }

    *outType = (MessageType)header.type;
    *outPayload = payload;
    *outPayloadLength = header.rawLength;
    return true;
}

bool expectMessage(ProtocolLink* link, MessageType expectedType, size_t minimumLength,
                   void** outPayload, uint32_t* outPayloadLength) {
    MessageType type;
    void* payload = NULL;
    uint32_t length = 0;
    if (!recvMessage(link, &type, &payload, &length)) return false;
    if (type != expectedType || length < minimumLength) {
        fprintf(stderr, "protocol: expected %s, got %s (%u bytes)\n",
                messageTypeName(expectedType), messageTypeName(type), length);
        free(payload);
        return false;
    }
    *outPayload = payload;
    *outPayloadLength = length;
    return true;
}

const char* messageTypeName(MessageType type) {
    switch (type) {
        case MSG_HELLO: return "HELLO";
        case MSG_HELLO_ACK: return "HELLO_ACK";
        case MSG_AUTH: return "AUTH";
        case MSG_AUTH_ACK: return "AUTH_ACK";
        case MSG_WATCH_ROOTS: return "WATCH_ROOTS";
        case MSG_WATCH_ROOTS_ACK: return "WATCH_ROOTS_ACK";
        case MSG_DIR_META: return "DIR_META";
        case MSG_DIR_META_ACK: return "DIR_META_ACK";
        case MSG_FILE_MANIFEST: return "FILE_MANIFEST";
        case MSG_FILE_NEEDED: return "FILE_NEEDED";
        case MSG_CHUNK_DATA: return "CHUNK_DATA";
        case MSG_CHUNK_ACK: return "CHUNK_ACK";
        case MSG_FILE_FINISH: return "FILE_FINISH";
        case MSG_FILE_RESULT: return "FILE_RESULT";
        case MSG_BYE: return "BYE";
        default: return "UNKNOWN";
    }
}
