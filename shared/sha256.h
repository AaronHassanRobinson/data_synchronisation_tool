//
// Minimal, self-contained SHA-256 (FIPS 180-4). Vendored instead of pulling
// in a crypto library, since this repo doesn't otherwise link against one
// and CDC only needs a stable content hash for chunk/file identity - not a
// hardened crypto implementation.
//
#ifndef DATA_SYNCHRONISATION_TOOL_SHA256_H
#define DATA_SYNCHRONISATION_TOOL_SHA256_H
#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitLength;
    uint8_t buffer[64];
    size_t bufferLength;
} Sha256Context;

void sha256Init(Sha256Context* ctx);
void sha256Update(Sha256Context* ctx, const void* data, size_t length);
void sha256Final(Sha256Context* ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

// Convenience: hash a single buffer in one call.
void sha256Buffer(const void* data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE]);

// Convenience: render a digest as a lowercase hex string (needs 65 bytes incl. the null terminator).
void sha256ToHex(const uint8_t digest[SHA256_DIGEST_SIZE], char outHex[65]);

#endif //DATA_SYNCHRONISATION_TOOL_SHA256_H
