#include "compress.h"
#include <stdlib.h>

#ifdef SYNC_HAVE_ZSTD
#include <zstd.h>

bool compressionAvailable(void) { return true; }

uint8_t* compressBuffer(const uint8_t* input, size_t inputLength, size_t* outLength) {
    const size_t bound = ZSTD_compressBound(inputLength);
    uint8_t* out = malloc(bound > 0 ? bound : 1);
    if (!out) return NULL;
    const size_t written = ZSTD_compress(out, bound, input, inputLength, 3);
    if (ZSTD_isError(written)) { free(out); return NULL; }
    *outLength = written;
    return out;
}

uint8_t* decompressBuffer(const uint8_t* input, size_t inputLength, size_t expectedRawLength) {
    uint8_t* out = malloc(expectedRawLength > 0 ? expectedRawLength : 1);
    if (!out) return NULL;
    const size_t got = ZSTD_decompress(out, expectedRawLength, input, inputLength);
    if (ZSTD_isError(got) || got != expectedRawLength) { free(out); return NULL; }
    return out;
}

#else

bool compressionAvailable(void) { return false; }
uint8_t* compressBuffer(const uint8_t* input, size_t inputLength, size_t* outLength) {
    (void)input; (void)inputLength; (void)outLength;
    return NULL;
}
uint8_t* decompressBuffer(const uint8_t* input, size_t inputLength, size_t expectedRawLength) {
    (void)input; (void)inputLength; (void)expectedRawLength;
    return NULL;
}

#endif
