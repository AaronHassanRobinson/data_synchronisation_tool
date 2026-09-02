#ifndef DATA_SYNCHRONISATION_TOOL_FILE_UTIL_H
#define DATA_SYNCHRONISATION_TOOL_FILE_UTIL_H
#include <stddef.h>
#include <stdint.h>

#define SYNC_PATH_MAX 4096

typedef struct {
    uint64_t size;
    uint64_t mtimeNs;   // modification time, nanoseconds since epoch (second precision on Windows)
    uint64_t inode;     // 0 on platforms without inode numbers
    bool isDirectory;
} FileInfo;

bool statFile(const char* path, FileInfo* out);
bool fileExists(const char* path);
bool deleteFile(const char* path);

// Reads an entire file into a heap buffer. Caller frees *outData on success.
bool readFileBytes(const char* path, uint8_t** outData, size_t* outLength);

// Reads `length` bytes starting at `offset` into `out` (used to fetch a single chunk's bytes).
bool readFileRange(const char* path, uint64_t offset, uint32_t length, uint8_t* out);

// Writes `data` to `path` atomically: sibling ".tmp" file first, then rename into place,
// so a crash or kill mid-write never leaves a half-written file at `path`.
bool writeFileBytesAtomic(const char* path, const uint8_t* data, size_t length);

// The design's "tainted read" guard: stat before and after the read; if mtime or size moved
// underneath us, someone was writing the file concurrently and the bytes can't be trusted.
typedef enum {
    GUARDED_READ_OK = 0,
    GUARDED_READ_MISSING,
    GUARDED_READ_TAINTED,
    GUARDED_READ_ERROR,
} GuardedReadStatus;
GuardedReadStatus readFileGuarded(const char* path, uint8_t** outData, size_t* outLength, FileInfo* outInfo);

// Pure comparison used by readFileGuarded and the chunker; exposed so it can be unit tested.
bool fileInfoUnchanged(const FileInfo* before, const FileInfo* after);

// mkdir -p. Returns true if every component now exists.
bool mkdirRecursive(const char* path);

// Joins two path pieces with exactly one separator between them. Returns false if it won't fit.
bool pathJoin(char* out, size_t capacity, const char* a, const char* b);

// Recursively walks `root`, calling `visit` for every file and directory beneath it (not `root`
// itself). `relPath` is relative to root and always uses '/' separators, so it's safe to put on
// the wire as-is. Return false from `visit` to stop the walk early.
typedef bool (*WalkVisitor)(const char* fullPath, const char* relPath, const FileInfo* info, void* userData);
bool walkDirectory(const char* root, WalkVisitor visit, void* userData);

// A relative path received off the wire is only safe to join onto an output directory if it
// can't climb out of it: no leading separator, no drive letter, no ".." component, no NULs.
bool isSafeRelativePath(const char* relPath);

// Copies `relPath` into `out` with '/' replaced by the platform separator.
void relPathToNative(const char* relPath, char* out, size_t capacity);

#endif //DATA_SYNCHRONISATION_TOOL_FILE_UTIL_H
