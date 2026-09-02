//
// Wire protocol. Every message is [ProtocolHeader][payload]; the payload may be zstd-compressed
// when both sides negotiated it in HELLO. Multi-byte integers are little-endian on the wire
// (every target platform - x86-64 and ARM64 on Linux/Windows/macOS - is little-endian, and the
// packed structs below are memcpy'd straight to/from the wire).
//
// Session flow (client drives everything; one connection, strictly request/response):
//
//   HELLO -> HELLO_ACK          version + capability negotiation, server issues an auth nonce
//   AUTH -> AUTH_ACK            mutual HMAC-SHA256 challenge/response over the pre-shared key
//   WATCH_ROOTS -> _ACK         client tells the server which root directories it's syncing
//   then, repeatedly:
//     DIR_META -> DIR_META_ACK          a directory record (server mkdir -p's it)
//     FILE_MANIFEST -> FILE_NEEDED      chunk layout of a file; server says which chunks it lacks
//     CHUNK_DATA x N -> CHUNK_ACK x N   only the missing chunks; each one individually acked
//     FILE_FINISH -> FILE_RESULT        server reconstructs + verifies whole-file hash
//   BYE
//
#ifndef DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
#define DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include "sha256.h"
#include "transport.h"

#define PROTOCOL_MAGIC 0x434E5953u // "SYNC" little-endian
#define PROTOCOL_VERSION_MIN 1
#define PROTOCOL_VERSION_MAX 1
#define PROTOCOL_MAX_PAYLOAD (64u * 1024u * 1024u)
#define PROTOCOL_NONCE_SIZE 32
#define PROTOCOL_MAX_CLIENT_ID 64

typedef enum {
    MSG_HELLO           = 1,
    MSG_HELLO_ACK       = 2,
    MSG_AUTH            = 3,
    MSG_AUTH_ACK        = 4,
    MSG_WATCH_ROOTS     = 5,
    MSG_WATCH_ROOTS_ACK = 6,
    MSG_DIR_META        = 7,
    MSG_DIR_META_ACK    = 8,
    MSG_FILE_MANIFEST   = 9,
    MSG_FILE_NEEDED     = 10,
    MSG_CHUNK_DATA      = 11,
    MSG_CHUNK_ACK       = 12,
    MSG_FILE_FINISH     = 13,
    MSG_FILE_RESULT     = 14,
    MSG_BYE             = 15,
} MessageType;

#define MSG_FLAG_COMPRESSED 0x0001

// Capability bits exchanged in HELLO / HELLO_ACK.
#define CAP_ZSTD 0x00000001u

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t flags;
    uint32_t rawLength;  // payload length after decompression
    uint32_t wireLength; // payload bytes that actually follow this header
} ProtocolHeader;

// The CDC parameters are agreed in HELLO so the server chunks pre-existing files identically.
typedef struct {
    uint32_t minChunkSize;
    uint32_t maxChunkSize;
    uint32_t maskBits;
} CdcParamsWire;

typedef struct {
    uint16_t minVersion;
    uint16_t maxVersion;
    uint32_t capabilities;
    CdcParamsWire cdc;
    uint32_t clientIdLength; // followed by clientId bytes
} HelloHeader;

typedef struct {
    uint16_t chosenVersion; // 0 = no common version, connection will be closed
    uint16_t reserved;
    uint32_t capabilities;  // intersection of both sides' capabilities
    uint8_t nonce[PROTOCOL_NONCE_SIZE];
} HelloAckHeader;

typedef struct {
    uint8_t mac[SHA256_DIGEST_SIZE];         // HMAC(psk, serverNonce || clientId)
    uint8_t clientNonce[PROTOCOL_NONCE_SIZE];
} AuthHeader;

typedef struct {
    uint8_t success;
    uint8_t mac[SHA256_DIGEST_SIZE];         // HMAC(psk, clientNonce || "server"), proves the server knows the key too
} AuthAckHeader;

typedef struct {
    uint32_t rootCount; // followed by rootCount * [uint32_t length][label bytes]
} WatchRootsHeader;

typedef struct {
    uint8_t ok;
} AckHeader;

typedef struct {
    uint32_t rootIndex;
    uint64_t mtimeSeconds;
    uint32_t pathLength;  // followed by relative path bytes ('/'-separated)
} DirMetaHeader;

typedef struct {
    uint64_t offset;
    uint32_t length;
    uint8_t hash[SHA256_DIGEST_SIZE];
} CdcChunkDescriptor;

typedef struct {
    uint32_t rootIndex;
    uint32_t pathLength;
    uint64_t fileSize;
    uint64_t mtimeSeconds;
    uint32_t chunkCount;
    uint8_t fileHash[SHA256_DIGEST_SIZE];
    // followed by: path bytes, then chunkCount * CdcChunkDescriptor
} FileManifestHeader;

typedef struct {
    uint32_t neededCount; // followed by neededCount * uint32_t chunk indices
} FileNeededHeader;

typedef struct {
    uint32_t chunkIndex;
    uint32_t length;      // followed by the chunk bytes
} ChunkDataHeader;

typedef struct {
    uint32_t chunkIndex;
    uint8_t ok;           // 0 = hash mismatch or write failure: client should resend
} ChunkAckHeader;

typedef enum {
    FILE_RESULT_OK = 0,
    FILE_RESULT_MISSING_CHUNKS = 1,
    FILE_RESULT_HASH_MISMATCH = 2,
    FILE_RESULT_WRITE_FAILED = 3,
    FILE_RESULT_REJECTED = 4,
} FileResultReason;

typedef struct {
    uint8_t success;
    uint8_t reason;       // FileResultReason
} FileResultHeader;

#pragma pack(pop)

// A framed, optionally-compressing message channel over a Transport.
typedef struct {
    Transport* transport;
    bool compressionEnabled; // set after HELLO negotiation
} ProtocolLink;

bool sendMessage(ProtocolLink* link, MessageType type, const void* payload, uint32_t payloadLength);

// Blocks for the next message; mallocs *outPayload (caller frees, may be NULL when empty).
bool recvMessage(ProtocolLink* link, MessageType* outType, void** outPayload, uint32_t* outPayloadLength);

// recvMessage + type check + minimum size check in one call. Frees the payload itself on failure.
bool expectMessage(ProtocolLink* link, MessageType expectedType, size_t minimumLength,
                   void** outPayload, uint32_t* outPayloadLength);

const char* messageTypeName(MessageType type);

#endif //DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
