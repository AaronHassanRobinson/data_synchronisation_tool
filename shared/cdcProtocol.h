//
// Builds and parses the payload bytes for the CDC_* message types declared
// in protocol.h. Kept separate from protocol.c (generic framing) and cdc.c
// (the chunking algorithm) since this is purely "structs <-> bytes".
//
#ifndef DATA_SYNCHRONISATION_TOOL_CDC_PROTOCOL_H
#define DATA_SYNCHRONISATION_TOOL_CDC_PROTOCOL_H
#include "cdc.h"

// Builds a MSG_CDC_MANIFEST payload from a chunk set + file name.
// Caller must free() the returned buffer.
void* buildManifestPayload(const char* fileName, uint32_t fileSize, const CdcChunkSet* chunkSet, uint32_t* outLength);

// Parses a MSG_CDC_MANIFEST payload. `outFileName` must have room for
// `fileNameCapacity` bytes including the null terminator. `outDescriptors`
// is set to point *inside* `payload` (no copy) - only valid while payload lives.
bool parseManifestPayload(const void* payload, uint32_t payloadLength,
                           char* outFileName, size_t fileNameCapacity,
                           uint32_t* outFileSize, uint8_t outFileHash[SHA256_DIGEST_SIZE],
                           uint32_t* outChunkCount, const CdcChunkDescriptor** outDescriptors);

// Builds a MSG_CDC_NEEDED_CHUNKS payload from an array of chunk indices.
// Caller must free() the returned buffer.
void* buildNeededChunksPayload(const uint32_t* indices, uint32_t count, uint32_t* outLength);

// Parses a MSG_CDC_NEEDED_CHUNKS payload. `outIndices` points inside `payload`.
bool parseNeededChunksPayload(const void* payload, uint32_t payloadLength,
                               const uint32_t** outIndices, uint32_t* outCount);

// Builds a MSG_CDC_CHUNK_DATA payload containing the raw bytes of the
// requested chunk indices, read out of `fileData` using the offset/length
// recorded for each index in `descriptors`. Caller must free() the result.
void* buildChunkDataPayload(const uint8_t* fileData, const CdcChunkDescriptor* descriptors,
                             const uint32_t* neededIndices, uint32_t neededCount, uint32_t* outLength);

// Called once per entry while parsing a MSG_CDC_CHUNK_DATA payload; `data`
// points inside the payload buffer and is only valid for the call's duration.
typedef void (*ChunkDataVisitor)(uint32_t chunkIndex, const uint8_t* data, uint32_t length, void* userData);
bool parseChunkDataPayload(const void* payload, uint32_t payloadLength, ChunkDataVisitor visit, void* userData);

#endif //DATA_SYNCHRONISATION_TOOL_CDC_PROTOCOL_H
