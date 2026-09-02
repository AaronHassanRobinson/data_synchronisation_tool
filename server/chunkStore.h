//
// Content-addressed chunk store: every chunk the server has ever verified lives at
// <store>/<hh>/<hash>. It is what makes the server side of CDC work:
//   - "which chunks do I need?" is a set of existence checks, never a byte comparison
//   - an interrupted transfer resumes where it left off: chunks acked before the drop are
//     already here, so the next manifest for that file only asks for the rest
//   - identical chunks shared between files are stored (and transferred) once
//
#ifndef DATA_SYNCHRONISATION_TOOL_CHUNK_STORE_H
#define DATA_SYNCHRONISATION_TOOL_CHUNK_STORE_H
#include <stdint.h>
#include "sha256.h"
#include "fileUtil.h"

typedef struct ChunkStore ChunkStore;

ChunkStore* chunkStoreOpen(const char* directory); // creates the directory tree if needed
void chunkStoreClose(ChunkStore* store);
bool chunkStoreHas(const ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE]);
// Verifies sha256(data) == hash before storing; returns false (stores nothing) otherwise.
bool chunkStorePut(ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE], const uint8_t* data, uint32_t length);
// Reads a chunk into `out`; the stored chunk must be exactly `length` bytes.
bool chunkStoreRead(const ChunkStore* store, const uint8_t hash[SHA256_DIGEST_SIZE], uint8_t* out, uint32_t length);

#endif //DATA_SYNCHRONISATION_TOOL_CHUNK_STORE_H
