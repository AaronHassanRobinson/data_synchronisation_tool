//
// zstd wrapper. The design compresses serialised messages just before they hit the transport;
// this keeps that a two-function affair and lets the build succeed without libzstd (in which
// case compression is simply never negotiated).
//
#ifndef DATA_SYNCHRONISATION_TOOL_COMPRESS_H
#define DATA_SYNCHRONISATION_TOOL_COMPRESS_H
#include <stddef.h>
#include <stdint.h>

bool compressionAvailable(void);

// Returns a malloc'd compressed buffer, or NULL if compression is unavailable or failed.
uint8_t* compressBuffer(const uint8_t* input, size_t inputLength, size_t* outLength);

// Returns a malloc'd buffer of exactly `expectedRawLength` bytes, or NULL on failure/mismatch.
uint8_t* decompressBuffer(const uint8_t* input, size_t inputLength, size_t expectedRawLength);

#endif //DATA_SYNCHRONISATION_TOOL_COMPRESS_H
