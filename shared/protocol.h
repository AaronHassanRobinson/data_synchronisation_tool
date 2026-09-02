//
// Created by MacbookPro on 25/8/2026.
//
// Wire protocol for the CDC sync demo: a version handshake followed by a
// simple request/response exchange per file (manifest -> needed chunks ->
// chunk data -> completion). Kept on plain TCP for this demo - the design
// doc's QUIC/compression/DB layers are deliberately out of scope here.
#ifndef DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
#define DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include "sha256.h"

#pragma pack(push, 1)

typedef enum {
    MSG_CDC_MANIFEST      = 1, // client -> server: file name + content-defined chunk layout
    MSG_CDC_NEEDED_CHUNKS = 2, // server -> client: which chunk indices it wants sent
    MSG_CDC_CHUNK_DATA    = 3, // client -> server: raw bytes for the requested chunk indices
    MSG_CDC_SYNC_COMPLETE = 4, // server -> client: whole-file hash verification result
} MessageType;

typedef struct {
    MessageType type;
    uint32_t length; // length in bytes of the payload that follows this header
} ProtocolHeader;

typedef enum {
    PROTOCOL_VERSION_CDC_V1 = 1,
} ProtocolVersion;

// First thing exchanged on a new connection, in both directions - lets the
// client and server confirm they speak the same protocol before anything
// else happens (a stand-in for the design's full version negotiation).
typedef struct {
    ProtocolVersion version;
} InitialExchangeHeader;

// --- MSG_CDC_MANIFEST payload layout ---
// [CdcManifestHeader][fileName bytes, nameLength][CdcChunkDescriptor * chunkCount]
typedef struct {
    uint32_t nameLength;
    uint32_t fileSize;
    uint32_t chunkCount;
    uint8_t fileHash[SHA256_DIGEST_SIZE]; // whole-file hash, for the final integrity check
} CdcManifestHeader;

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint8_t hash[SHA256_DIGEST_SIZE];
} CdcChunkDescriptor;

// --- MSG_CDC_NEEDED_CHUNKS payload layout ---
// [CdcNeededHeader][uint32_t chunkIndex * neededCount]
typedef struct {
    uint32_t neededCount;
} CdcNeededHeader;

// --- MSG_CDC_CHUNK_DATA payload layout ---
// [CdcChunkDataHeader][ (CdcChunkDataEntry, then `length` raw bytes) * chunkCount ]
typedef struct {
    uint32_t chunkCount;
} CdcChunkDataHeader;

typedef struct {
    uint32_t chunkIndex;
    uint32_t length;
} CdcChunkDataEntry;

// --- MSG_CDC_SYNC_COMPLETE payload layout ---
typedef struct {
    uint8_t success; // 1 once the server has rebuilt the file and its hash matches the manifest
} CdcSyncCompleteHeader;

#pragma pack(pop)

// Reliable framing helpers for a connected TCP socket. A single send()/recv()
// call is not guaranteed to move the whole buffer, so every message send and
// receive in this project goes through these instead of calling send/recv directly.
bool sendAll(int socketFd, const void* data, size_t length);
bool recvAll(int socketFd, void* buffer, size_t length);

// Sends [ProtocolHeader][payload] as one logical message.
bool sendMessage(int socketFd, MessageType type, const void* payload, uint32_t payloadLength);

// Blocks for the next message header, then mallocs and reads its payload.
// Caller owns *outPayload and must free() it (even on a successful call with
// a zero-length payload, where it may be NULL). Returns false on disconnect/error.
bool recvMessage(int socketFd, MessageType* outType, void** outPayload, uint32_t* outPayloadLength);

#endif //DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
