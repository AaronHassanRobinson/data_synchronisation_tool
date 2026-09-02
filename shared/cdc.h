//
// Content-defined chunking (CDC): splits a buffer into variable-length
// chunks at boundaries chosen by a rolling hash over the content, rather
// than at fixed offsets - so inserting/deleting a byte near the start of a
// file only changes the chunk(s) around the edit, not every chunk after it
// (which is what would happen with fixed-size blocks).
//
// This is a deliberately small FastCDC-style chunker (gear-hash rolling
// hash, min/max chunk size, no normalized chunking) - enough to demonstrate
// the idea end to end, not a tuned production implementation.
//
#ifndef DATA_SYNCHRONISATION_TOOL_CDC_H
#define DATA_SYNCHRONISATION_TOOL_CDC_H
#include <stddef.h>
#include "protocol.h" // for CdcChunkDescriptor - the wire format doubles as our in-memory chunk record

// Chunk size bounds, kept small so the demo's example files (a few KB)
// actually produce several chunks. Real deployments would size these in
// KB-MB, per the design doc's "configurable" chunk hash / size note.
#define CDC_MIN_CHUNK_SIZE 64
#define CDC_MAX_CHUNK_SIZE 2048
#define CDC_MASK_BITS 8 // boundary probability ~= 1/2^CDC_MASK_BITS -> ~256 byte average chunk

typedef struct {
    CdcChunkDescriptor* chunks; // heap array, length chunkCount
    uint32_t chunkCount;
    uint8_t fileHash[SHA256_DIGEST_SIZE]; // hash of the whole buffer, for end-to-end verification
} CdcChunkSet;

// Splits `data` (length bytes) into content-defined chunks, hashing each
// chunk and the buffer as a whole. Caller must free the result with cdcFreeChunkSet.
CdcChunkSet cdcChunkBuffer(const uint8_t* data, size_t length);
void cdcFreeChunkSet(CdcChunkSet* set);

#endif //DATA_SYNCHRONISATION_TOOL_CDC_H
