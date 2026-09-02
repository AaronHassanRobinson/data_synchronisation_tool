#include "cdcProtocol.h"
#include <stdlib.h>
#include <string.h>

void* buildManifestPayload(const char* fileName, const uint32_t fileSize, const CdcChunkSet* chunkSet, uint32_t* outLength) {
    const uint32_t nameLength = (uint32_t)strlen(fileName);
    const uint32_t total = sizeof(CdcManifestHeader) + nameLength + chunkSet->chunkCount * (uint32_t)sizeof(CdcChunkDescriptor);
    uint8_t* buffer = malloc(total);
    uint8_t* cursor = buffer;

    CdcManifestHeader header = {
        .nameLength = nameLength,
        .fileSize = fileSize,
        .chunkCount = chunkSet->chunkCount,
    };
    memcpy(header.fileHash, chunkSet->fileHash, SHA256_DIGEST_SIZE);
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    memcpy(cursor, fileName, nameLength);
    cursor += nameLength;

    memcpy(cursor, chunkSet->chunks, chunkSet->chunkCount * sizeof(CdcChunkDescriptor));

    *outLength = total;
    return buffer;
}

bool parseManifestPayload(const void* payload, const uint32_t payloadLength,
                           char* outFileName, const size_t fileNameCapacity,
                           uint32_t* outFileSize, uint8_t outFileHash[SHA256_DIGEST_SIZE],
                           uint32_t* outChunkCount, const CdcChunkDescriptor** outDescriptors) {
    if (payloadLength < sizeof(CdcManifestHeader)) return false;

    const uint8_t* cursor = payload;
    CdcManifestHeader header;
    memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);

    const uint64_t expectedLength = (uint64_t)sizeof(header) + header.nameLength +
                                     (uint64_t)header.chunkCount * sizeof(CdcChunkDescriptor);
    if (payloadLength < expectedLength) return false;
    if ((uint64_t)header.nameLength + 1 > fileNameCapacity) return false;

    memcpy(outFileName, cursor, header.nameLength);
    outFileName[header.nameLength] = '\0';
    cursor += header.nameLength;

    *outFileSize = header.fileSize;
    memcpy(outFileHash, header.fileHash, SHA256_DIGEST_SIZE);
    *outChunkCount = header.chunkCount;
    *outDescriptors = (const CdcChunkDescriptor*)cursor;
    return true;
}

void* buildNeededChunksPayload(const uint32_t* indices, const uint32_t count, uint32_t* outLength) {
    const uint32_t total = sizeof(CdcNeededHeader) + count * (uint32_t)sizeof(uint32_t);
    uint8_t* buffer = malloc(total);

    const CdcNeededHeader header = {.neededCount = count};
    memcpy(buffer, &header, sizeof(header));
    if (count > 0) memcpy(buffer + sizeof(header), indices, count * sizeof(uint32_t));

    *outLength = total;
    return buffer;
}

bool parseNeededChunksPayload(const void* payload, const uint32_t payloadLength,
                               const uint32_t** outIndices, uint32_t* outCount) {
    if (payloadLength < sizeof(CdcNeededHeader)) return false;

    CdcNeededHeader header;
    memcpy(&header, payload, sizeof(header));

    const uint64_t expectedLength = sizeof(CdcNeededHeader) + (uint64_t)header.neededCount * sizeof(uint32_t);
    if (payloadLength < expectedLength) return false;

    *outCount = header.neededCount;
    *outIndices = (const uint32_t*)((const uint8_t*)payload + sizeof(CdcNeededHeader));
    return true;
}

void* buildChunkDataPayload(const uint8_t* fileData, const CdcChunkDescriptor* descriptors,
                             const uint32_t* neededIndices, const uint32_t neededCount, uint32_t* outLength) {
    uint32_t total = sizeof(CdcChunkDataHeader);
    for (uint32_t i = 0; i < neededCount; i++) {
        total += (uint32_t)sizeof(CdcChunkDataEntry) + descriptors[neededIndices[i]].length;
    }

    uint8_t* buffer = malloc(total);
    uint8_t* cursor = buffer;

    const CdcChunkDataHeader header = {.chunkCount = neededCount};
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    for (uint32_t i = 0; i < neededCount; i++) {
        const uint32_t index = neededIndices[i];
        const CdcChunkDataEntry entry = {.chunkIndex = index, .length = descriptors[index].length};
        memcpy(cursor, &entry, sizeof(entry));
        cursor += sizeof(entry);
        memcpy(cursor, fileData + descriptors[index].offset, descriptors[index].length);
        cursor += descriptors[index].length;
    }

    *outLength = total;
    return buffer;
}

bool parseChunkDataPayload(const void* payload, const uint32_t payloadLength, const ChunkDataVisitor visit, void* userData) {
    if (payloadLength < sizeof(CdcChunkDataHeader)) return false;

    const uint8_t* cursor = payload;
    const uint8_t* end = cursor + payloadLength;

    CdcChunkDataHeader header;
    memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);

    for (uint32_t i = 0; i < header.chunkCount; i++) {
        if ((size_t)(end - cursor) < sizeof(CdcChunkDataEntry)) return false;
        CdcChunkDataEntry entry;
        memcpy(&entry, cursor, sizeof(entry));
        cursor += sizeof(entry);

        if ((size_t)(end - cursor) < entry.length) return false;
        visit(entry.chunkIndex, cursor, entry.length, userData);
        cursor += entry.length;
    }
    return true;
}
