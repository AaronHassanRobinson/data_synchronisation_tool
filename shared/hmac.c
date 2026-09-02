#include "hmac.h"
#include <string.h>
#include <stdio.h>

void hmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* message, size_t messageLength,
                uint8_t outMac[SHA256_DIGEST_SIZE]) {
    uint8_t keyBlock[64] = {0};
    if (keyLength > 64) {
        sha256Buffer(key, keyLength, keyBlock);
    } else {
        memcpy(keyBlock, key, keyLength);
    }

    uint8_t innerPad[64], outerPad[64];
    for (int i = 0; i < 64; i++) {
        innerPad[i] = keyBlock[i] ^ 0x36;
        outerPad[i] = keyBlock[i] ^ 0x5c;
    }

    uint8_t innerHash[SHA256_DIGEST_SIZE];
    Sha256Context ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, innerPad, 64);
    sha256Update(&ctx, message, messageLength);
    sha256Final(&ctx, innerHash);

    sha256Init(&ctx);
    sha256Update(&ctx, outerPad, 64);
    sha256Update(&ctx, innerHash, SHA256_DIGEST_SIZE);
    sha256Final(&ctx, outMac);
}

bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
bool randomBytes(uint8_t* out, size_t length) {
    return BCryptGenRandom(NULL, out, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
#else
bool randomBytes(uint8_t* out, size_t length) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t got = fread(out, 1, length, f);
    fclose(f);
    return got == length;
}
#endif
