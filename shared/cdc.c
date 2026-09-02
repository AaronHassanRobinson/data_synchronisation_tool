#include "cdc.h"
#include <stdlib.h>
#include <string.h>

// Gear table: 256 pseudo-random 64-bit values the rolling hash folds each byte through
// (`hash = (hash << 1) + gear[byte]`), approximating a hash of the last ~64 bytes without an
// explicit sliding window. Generated once from a fixed seed via splitmix64, so every build on
// every platform derives the identical table.
static uint64_t gearTable[256];
static bool gearTableReady = false;

static uint64_t splitmix64Next(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void ensureGearTable(void) {
    if (gearTableReady) return;
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 256; i++) gearTable[i] = splitmix64Next(&seed);
    gearTableReady = true;
}

bool cdcParamsValid(const CdcParams* p) {
    return p->minChunkSize >= 16 && p->maxChunkSize >= p->minChunkSize &&
           p->maxChunkSize <= PROTOCOL_MAX_PAYLOAD / 2 && p->maskBits >= 4 && p->maskBits <= 40;
}

CdcParams cdcParamsFromWire(const CdcParamsWire* wire) {
    return (CdcParams){ .minChunkSize = wire->minChunkSize, .maxChunkSize = wire->maxChunkSize, .maskBits = wire->maskBits };
}

CdcParamsWire cdcParamsToWire(const CdcParams* p) {
    return (CdcParamsWire){ .minChunkSize = p->minChunkSize, .maxChunkSize = p->maxChunkSize, .maskBits = p->maskBits };
}

void cdcChunkerInit(CdcChunker* c, const CdcParams* params, CdcChunkSet* output) {
    ensureGearTable();
    memset(c, 0, sizeof(*c));
    c->params = *params;
    c->output = output;
    memset(output, 0, sizeof(*output));
    sha256Init(&c->chunkHash);
    sha256Init(&c->fileHash);
}

static void emitChunk(CdcChunker* c) {
    CdcChunkSet* out = c->output;
    if (out->chunkCount == c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 64;
        out->chunks = realloc(out->chunks, c->capacity * sizeof(CdcChunkDescriptor));
    }
    CdcChunkDescriptor* chunk = &out->chunks[out->chunkCount++];
    chunk->offset = c->chunkStart;
    chunk->length = (uint32_t)(c->position - c->chunkStart);
    sha256Final(&c->chunkHash, chunk->hash);

    sha256Init(&c->chunkHash);
    c->chunkStart = c->position;
    c->rollingHash = 0;
}

void cdcChunkerFeed(CdcChunker* c, const uint8_t* data, size_t length) {
    const uint64_t mask = (1ULL << c->params.maskBits) - 1;
    size_t runStart = 0; // bytes [runStart, i) belong to the current chunk and haven't been hashed yet

    for (size_t i = 0; i < length; i++) {
        c->rollingHash = (c->rollingHash << 1) + gearTable[data[i]];
        const uint64_t chunkLength = c->position - c->chunkStart + 1;

        if (chunkLength >= c->params.maxChunkSize ||
            (chunkLength >= c->params.minChunkSize && (c->rollingHash & mask) == 0)) {
            sha256Update(&c->chunkHash, data + runStart, i + 1 - runStart);
            c->position++;
            emitChunk(c);
            runStart = i + 1;
        } else {
            c->position++;
        }
    }
    if (runStart < length) sha256Update(&c->chunkHash, data + runStart, length - runStart);
    sha256Update(&c->fileHash, data, length);
}

void cdcChunkerFinish(CdcChunker* c) {
    if (c->position > c->chunkStart) emitChunk(c);
    c->output->totalLength = c->position;
    sha256Final(&c->fileHash, c->output->fileHash);
}

CdcChunkSet cdcChunkBuffer(const uint8_t* data, size_t length, const CdcParams* params) {
    CdcChunkSet set;
    CdcChunker chunker;
    cdcChunkerInit(&chunker, params, &set);
    cdcChunkerFeed(&chunker, data, length);
    cdcChunkerFinish(&chunker);
    return set;
}

bool cdcChunkFile(const char* path, const CdcParams* params, CdcChunkSet* out) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;

    CdcChunker chunker;
    cdcChunkerInit(&chunker, params, out);

    enum { BUFFER_SIZE = 1024 * 1024 };
    uint8_t* buffer = malloc(BUFFER_SIZE);
    size_t got;
    while ((got = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        cdcChunkerFeed(&chunker, buffer, got);
    }
    const bool ok = !ferror(file);
    free(buffer);
    fclose(file);

    cdcChunkerFinish(&chunker);
    if (!ok) cdcFreeChunkSet(out);
    return ok;
}

void cdcFreeChunkSet(CdcChunkSet* set) {
    free(set->chunks);
    set->chunks = NULL;
    set->chunkCount = 0;
}
