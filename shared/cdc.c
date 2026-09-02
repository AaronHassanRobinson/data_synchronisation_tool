#include "cdc.h"
#include <stdlib.h>

// Gear table: 256 pseudo-random 64-bit values used to build a rolling hash
// over the byte stream (Xia et al.'s "gear hash", as used by FastCDC). Each
// byte folds into the hash as `hash = (hash << 1) + gearTable[byte]`, which
// approximates a hash of the last several bytes without needing to track an
// explicit sliding window.
//
// Generated once from a fixed seed via splitmix64 rather than hardcoded, so
// client and server (or a fresh build) always derive the identical table
// without shipping 256 magic numbers.
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
    uint64_t seed = 0x9E3779B97F4A7C15ULL; // fixed: same table every run, on every machine
    for (int i = 0; i < 256; i++) {
        gearTable[i] = splitmix64Next(&seed);
    }
    gearTableReady = true;
}

CdcChunkSet cdcChunkBuffer(const uint8_t* data, const size_t length) {
    ensureGearTable();

    CdcChunkSet result = {0};
    sha256Buffer(data, length, result.fileHash);
    if (length == 0) return result;

    // Worst case every chunk hits CDC_MIN_CHUNK_SIZE - safe upper bound on chunk count.
    const uint32_t capacity = (uint32_t)(length / CDC_MIN_CHUNK_SIZE) + 2;
    result.chunks = malloc(capacity * sizeof(CdcChunkDescriptor));

    const uint64_t mask = (1ULL << CDC_MASK_BITS) - 1;
    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = length - offset;
        const size_t maxLen = remaining < CDC_MAX_CHUNK_SIZE ? remaining : CDC_MAX_CHUNK_SIZE;

        uint64_t hash = 0;
        size_t boundary = maxLen; // fall back to cutting at the max size if no hash boundary is found
        for (size_t i = 0; i < maxLen; i++) {
            hash = (hash << 1) + gearTable[data[offset + i]];
            if (i + 1 >= CDC_MIN_CHUNK_SIZE && (hash & mask) == 0) {
                boundary = i + 1;
                break;
            }
        }

        CdcChunkDescriptor* chunk = &result.chunks[result.chunkCount++];
        chunk->offset = (uint32_t)offset;
        chunk->length = (uint32_t)boundary;
        sha256Buffer(data + offset, boundary, chunk->hash);

        offset += boundary;
    }

    return result;
}

void cdcFreeChunkSet(CdcChunkSet* set) {
    free(set->chunks);
    set->chunks = NULL;
    set->chunkCount = 0;
}
