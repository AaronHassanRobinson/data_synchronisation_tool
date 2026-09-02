//
// HMAC-SHA256 (RFC 2104) over the vendored SHA-256. Used for the client/server
// pre-shared-key challenge/response authentication - never for anything the
// transport's TLS already covers.
//
#ifndef DATA_SYNCHRONISATION_TOOL_HMAC_H
#define DATA_SYNCHRONISATION_TOOL_HMAC_H
#include <stddef.h>
#include <stdint.h>
#include "sha256.h"

void hmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* message, size_t messageLength,
                uint8_t outMac[SHA256_DIGEST_SIZE]);

// Constant-time comparison so an attacker can't time their way through a MAC byte by byte.
bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t length);

// Fills `out` with `length` bytes from the OS CSPRNG (/dev/urandom or BCryptGenRandom).
bool randomBytes(uint8_t* out, size_t length);

#endif //DATA_SYNCHRONISATION_TOOL_HMAC_H
