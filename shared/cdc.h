//
// Content-defined chunking (CDC). A file is split into variable-length chunks at boundaries
// chosen by a rolling hash over the *content*, not at fixed offsets, so inserting or deleting
// bytes near the start of a file only changes the chunk(s) around the edit - everything after it
// re-chunks identically (unlike fixed-size blocks, where every later block would shift).
//
// FastCDC-style: gear-hash rolling hash, boundary when the low `maskBits` bits are zero, with
// min/max chunk size bounds. Parameters are negotiated in HELLO so both ends chunk identically.
//
#ifndef DATA_SYNCHRONISATION_TOOL_CDC_H
#define DATA_SYNCHRONISATION_TOOL_CDC_H
#include <stddef.h>
#include <stdio.h>
#include "protocol.h" // CdcChunkDescriptor / CdcParamsWire

typedef struct {
    uint32_t minChunkSize;
    uint32_t maxChunkSize;
    uint32_t maskBits; // average chunk ~= 2^maskBits bytes
} CdcParams;

// Sensible production-ish defaults (~64 KiB average). The demo config overrides these with tiny
// values so small sample files visibly produce several chunks.
#define CDC_DEFAULT_MIN_CHUNK_SIZE (16u * 1024u)
#define CDC_DEFAULT_MAX_CHUNK_SIZE (256u * 1024u)
#define CDC_DEFAULT_MASK_BITS 16u

bool cdcParamsValid(const CdcParams* params);
CdcParams cdcParamsFromWire(const CdcParamsWire* wire);
CdcParamsWire cdcParamsToWire(const CdcParams* params);

typedef struct {
    CdcChunkDescriptor* chunks; // heap array, chunkCount entries
    uint32_t chunkCount;
    uint64_t totalLength;
    uint8_t fileHash[SHA256_DIGEST_SIZE]; // hash of all the bytes, for end-to-end verification
} CdcChunkSet;

// Incremental chunker: feed it bytes in any sized pieces, then finish. This is what lets the
// client chunk a multi-GB file without holding it in memory.
typedef struct {
    CdcParams params;
    uint64_t rollingHash;
    uint64_t chunkStart;
    uint64_t position;
    Sha256Context chunkHash;
    Sha256Context fileHash;
    CdcChunkSet* output;
    uint32_t capacity;
} CdcChunker;

void cdcChunkerInit(CdcChunker* chunker, const CdcParams* params, CdcChunkSet* output);
void cdcChunkerFeed(CdcChunker* chunker, const uint8_t* data, size_t length);
void cdcChunkerFinish(CdcChunker* chunker);

// Convenience wrappers around the incremental chunker.
CdcChunkSet cdcChunkBuffer(const uint8_t* data, size_t length, const CdcParams* params);
bool cdcChunkFile(const char* path, const CdcParams* params, CdcChunkSet* out); // streams from disk
void cdcFreeChunkSet(CdcChunkSet* set);

#endif //DATA_SYNCHRONISATION_TOOL_CDC_H
