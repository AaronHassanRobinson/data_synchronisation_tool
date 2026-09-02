#include "cdcProtocol.h"
#include "fileUtil.h"
#include <stdlib.h>
#include <string.h>

// Small cursor helpers so every builder/parser reads the same way.
typedef struct { uint8_t* base; uint8_t* cursor; } Writer;
typedef struct { const uint8_t* cursor; const uint8_t* end; } Reader;

static void put(Writer* w, const void* data, size_t length) { memcpy(w->cursor, data, length); w->cursor += length; }
static bool take(Reader* r, void* out, size_t length) {
    if ((size_t)(r->end - r->cursor) < length) return false;
    memcpy(out, r->cursor, length);
    r->cursor += length;
    return true;
}
static bool takeString(Reader* r, uint32_t length, char* out, size_t capacity) {
    if ((size_t)(r->end - r->cursor) < length || (size_t)length + 1 > capacity) return false;
    memcpy(out, r->cursor, length);
    out[length] = '\0';
    r->cursor += length;
    return true;
}

// ---------------------------------------------------------------- HELLO

void* buildHelloPayload(const char* clientId, uint32_t capabilities, const CdcParams* cdc, uint32_t* outLength) {
    const uint32_t idLength = (uint32_t)strlen(clientId);
    const HelloHeader header = {
        .minVersion = PROTOCOL_VERSION_MIN, .maxVersion = PROTOCOL_VERSION_MAX,
        .capabilities = capabilities, .cdc = cdcParamsToWire(cdc), .clientIdLength = idLength,
    };
    *outLength = sizeof(header) + idLength;
    Writer w = { .base = malloc(*outLength) };
    w.cursor = w.base;
    put(&w, &header, sizeof(header));
    put(&w, clientId, idLength);
    return w.base;
}

bool parseHelloPayload(const void* payload, uint32_t length, HelloHeader* outHeader, char* outClientId, size_t capacity) {
    Reader r = { payload, (const uint8_t*)payload + length };
    if (!take(&r, outHeader, sizeof(*outHeader))) return false;
    if (outHeader->clientIdLength == 0 || outHeader->clientIdLength > PROTOCOL_MAX_CLIENT_ID) return false;
    return takeString(&r, outHeader->clientIdLength, outClientId, capacity);
}

// ---------------------------------------------------------------- WATCH_ROOTS

void* buildWatchRootsPayload(const char* const* labels, uint32_t count, uint32_t* outLength) {
    uint32_t total = sizeof(WatchRootsHeader);
    for (uint32_t i = 0; i < count; i++) total += sizeof(uint32_t) + (uint32_t)strlen(labels[i]);
    Writer w = { .base = malloc(total) };
    w.cursor = w.base;
    const WatchRootsHeader header = { .rootCount = count };
    put(&w, &header, sizeof(header));
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t labelLength = (uint32_t)strlen(labels[i]);
        put(&w, &labelLength, sizeof(labelLength));
        put(&w, labels[i], labelLength);
    }
    *outLength = total;
    return w.base;
}

bool parseWatchRootsPayload(const void* payload, uint32_t length, RootLabelVisitor visit, void* userData) {
    Reader r = { payload, (const uint8_t*)payload + length };
    WatchRootsHeader header;
    if (!take(&r, &header, sizeof(header)) || header.rootCount > 1024) return false;
    for (uint32_t i = 0; i < header.rootCount; i++) {
        uint32_t labelLength;
        char label[256];
        if (!take(&r, &labelLength, sizeof(labelLength)) || !takeString(&r, labelLength, label, sizeof(label))) return false;
        if (!visit(i, label, userData)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- DIR_META

void* buildDirMetaPayload(uint32_t rootIndex, const char* relPath, uint64_t mtimeSeconds, uint32_t* outLength) {
    const uint32_t pathLength = (uint32_t)strlen(relPath);
    const DirMetaHeader header = { .rootIndex = rootIndex, .mtimeSeconds = mtimeSeconds, .pathLength = pathLength };
    *outLength = sizeof(header) + pathLength;
    Writer w = { .base = malloc(*outLength) };
    w.cursor = w.base;
    put(&w, &header, sizeof(header));
    put(&w, relPath, pathLength);
    return w.base;
}

bool parseDirMetaPayload(const void* payload, uint32_t length, DirMetaHeader* outHeader, char* outRelPath, size_t capacity) {
    Reader r = { payload, (const uint8_t*)payload + length };
    if (!take(&r, outHeader, sizeof(*outHeader))) return false;
    return takeString(&r, outHeader->pathLength, outRelPath, capacity);
}

// ---------------------------------------------------------------- FILE_MANIFEST

void* buildManifestPayload(uint32_t rootIndex, const char* relPath, uint64_t mtimeSeconds,
                           const CdcChunkSet* chunkSet, uint32_t* outLength) {
    const uint32_t pathLength = (uint32_t)strlen(relPath);
    FileManifestHeader header = {
        .rootIndex = rootIndex, .pathLength = pathLength, .fileSize = chunkSet->totalLength,
        .mtimeSeconds = mtimeSeconds, .chunkCount = chunkSet->chunkCount,
    };
    memcpy(header.fileHash, chunkSet->fileHash, SHA256_DIGEST_SIZE);

    *outLength = sizeof(header) + pathLength + chunkSet->chunkCount * (uint32_t)sizeof(CdcChunkDescriptor);
    Writer w = { .base = malloc(*outLength) };
    w.cursor = w.base;
    put(&w, &header, sizeof(header));
    put(&w, relPath, pathLength);
    if (chunkSet->chunkCount > 0) put(&w, chunkSet->chunks, chunkSet->chunkCount * sizeof(CdcChunkDescriptor));
    return w.base;
}

bool parseManifestPayload(const void* payload, uint32_t length, FileManifestHeader* outHeader,
                          char* outRelPath, size_t capacity, const CdcChunkDescriptor** outDescriptors) {
    Reader r = { payload, (const uint8_t*)payload + length };
    if (!take(&r, outHeader, sizeof(*outHeader))) return false;
    if (!takeString(&r, outHeader->pathLength, outRelPath, capacity)) return false;
    if ((uint64_t)(r.end - r.cursor) < (uint64_t)outHeader->chunkCount * sizeof(CdcChunkDescriptor)) return false;
    *outDescriptors = (const CdcChunkDescriptor*)r.cursor;

    // Descriptors must tile the file exactly, in order - anything else is a malformed/malicious manifest.
    uint64_t expectedOffset = 0;
    for (uint32_t i = 0; i < outHeader->chunkCount; i++) {
        if ((*outDescriptors)[i].offset != expectedOffset || (*outDescriptors)[i].length == 0) return false;
        expectedOffset += (*outDescriptors)[i].length;
    }
    return expectedOffset == outHeader->fileSize;
}

// ---------------------------------------------------------------- FILE_NEEDED

void* buildNeededPayload(const uint32_t* indices, uint32_t count, uint32_t* outLength) {
    *outLength = sizeof(FileNeededHeader) + count * (uint32_t)sizeof(uint32_t);
    Writer w = { .base = malloc(*outLength) };
    w.cursor = w.base;
    const FileNeededHeader header = { .neededCount = count };
    put(&w, &header, sizeof(header));
    if (count > 0) put(&w, indices, count * sizeof(uint32_t));
    return w.base;
}

bool parseNeededPayload(const void* payload, uint32_t length, const uint32_t** outIndices, uint32_t* outCount) {
    Reader r = { payload, (const uint8_t*)payload + length };
    FileNeededHeader header;
    if (!take(&r, &header, sizeof(header))) return false;
    if ((uint64_t)(r.end - r.cursor) < (uint64_t)header.neededCount * sizeof(uint32_t)) return false;
    *outCount = header.neededCount;
    *outIndices = (const uint32_t*)r.cursor;
    return true;
}

// ---------------------------------------------------------------- CHUNK_DATA

void* buildChunkDataPayload(uint32_t chunkIndex, const uint8_t* data, uint32_t dataLength, uint32_t* outLength) {
    *outLength = sizeof(ChunkDataHeader) + dataLength;
    Writer w = { .base = malloc(*outLength) };
    w.cursor = w.base;
    const ChunkDataHeader header = { .chunkIndex = chunkIndex, .length = dataLength };
    put(&w, &header, sizeof(header));
    put(&w, data, dataLength);
    return w.base;
}

bool parseChunkDataPayload(const void* payload, uint32_t length, uint32_t* outChunkIndex,
                           const uint8_t** outData, uint32_t* outDataLength) {
    Reader r = { payload, (const uint8_t*)payload + length };
    ChunkDataHeader header;
    if (!take(&r, &header, sizeof(header))) return false;
    if ((uint32_t)(r.end - r.cursor) != header.length) return false;
    *outChunkIndex = header.chunkIndex;
    *outData = r.cursor;
    *outDataLength = header.length;
    return true;
}
