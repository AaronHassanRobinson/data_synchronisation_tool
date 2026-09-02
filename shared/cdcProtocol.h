//
// Payload builders/parsers for the messages in protocol.h. Pure "structs <-> bytes"; no I/O.
// Every parser bounds-checks against the payload length, since payloads come off the wire.
// Parsers that return pointers (descriptors, indices, strings) point *into* the payload buffer.
//
#ifndef DATA_SYNCHRONISATION_TOOL_CDC_PROTOCOL_H
#define DATA_SYNCHRONISATION_TOOL_CDC_PROTOCOL_H
#include "cdc.h"

// HELLO
void* buildHelloPayload(const char* clientId, uint32_t capabilities, const CdcParams* cdc, uint32_t* outLength);
bool parseHelloPayload(const void* payload, uint32_t length, HelloHeader* outHeader,
                       char* outClientId, size_t clientIdCapacity);

// WATCH_ROOTS
void* buildWatchRootsPayload(const char* const* labels, uint32_t count, uint32_t* outLength);
// Calls `visit` per root label; labels are copied into a NUL-terminated scratch buffer per call.
typedef bool (*RootLabelVisitor)(uint32_t index, const char* label, void* userData);
bool parseWatchRootsPayload(const void* payload, uint32_t length, RootLabelVisitor visit, void* userData);

// DIR_META
void* buildDirMetaPayload(uint32_t rootIndex, const char* relPath, uint64_t mtimeSeconds, uint32_t* outLength);
bool parseDirMetaPayload(const void* payload, uint32_t length, DirMetaHeader* outHeader,
                         char* outRelPath, size_t relPathCapacity);

// FILE_MANIFEST
void* buildManifestPayload(uint32_t rootIndex, const char* relPath, uint64_t mtimeSeconds,
                           const CdcChunkSet* chunkSet, uint32_t* outLength);
bool parseManifestPayload(const void* payload, uint32_t length, FileManifestHeader* outHeader,
                          char* outRelPath, size_t relPathCapacity, const CdcChunkDescriptor** outDescriptors);

// FILE_NEEDED
void* buildNeededPayload(const uint32_t* indices, uint32_t count, uint32_t* outLength);
bool parseNeededPayload(const void* payload, uint32_t length, const uint32_t** outIndices, uint32_t* outCount);

// CHUNK_DATA
void* buildChunkDataPayload(uint32_t chunkIndex, const uint8_t* data, uint32_t dataLength, uint32_t* outLength);
bool parseChunkDataPayload(const void* payload, uint32_t length, uint32_t* outChunkIndex,
                           const uint8_t** outData, uint32_t* outDataLength);

#endif //DATA_SYNCHRONISATION_TOOL_CDC_PROTOCOL_H
