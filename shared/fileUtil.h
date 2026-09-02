#ifndef DATA_SYNCHRONISATION_TOOL_FILE_UTIL_H
#define DATA_SYNCHRONISATION_TOOL_FILE_UTIL_H
#include <stddef.h>
#include <stdint.h>

// Reads an entire file into a heap buffer. Caller frees *outData on success.
// Returns false (leaving *outData/*outLength untouched) if the file doesn't
// exist or can't be read - callers use this to mean "no local copy yet".
bool readFileBytes(const char* path, uint8_t** outData, size_t* outLength);

// Writes `data` to `path` atomically: writes to a sibling "<path>.tmp" file
// first, then rename()s it into place, so a crash or kill mid-write never
// leaves a half-written file at `path` - relevant to the design's requirement
// that the tool tolerate the host machine going down mid-transfer.
bool writeFileBytesAtomic(const char* path, const uint8_t* data, size_t length);

#endif //DATA_SYNCHRONISATION_TOOL_FILE_UTIL_H
