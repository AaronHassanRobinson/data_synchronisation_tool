#include "fileUtil.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
  #define STAT_STRUCT struct _stat64
  #define STAT_FN _stat64
  static uint64_t mtimeNsOf(const STAT_STRUCT* st) { return (uint64_t)st->st_mtime * 1000000000ULL; }
#else
  #define STAT_STRUCT struct stat
  #define STAT_FN stat
  #ifdef __APPLE__
    static uint64_t mtimeNsOf(const STAT_STRUCT* st) { return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ULL + (uint64_t)st->st_mtimespec.tv_nsec; }
  #else
    static uint64_t mtimeNsOf(const STAT_STRUCT* st) { return (uint64_t)st->st_mtim.tv_sec * 1000000000ULL + (uint64_t)st->st_mtim.tv_nsec; }
  #endif
#endif

bool statFile(const char* path, FileInfo* out) {
    STAT_STRUCT st;
    if (STAT_FN(path, &st) != 0) return false;
    out->size = (uint64_t)st.st_size;
    out->mtimeNs = mtimeNsOf(&st);
    out->inode = (uint64_t)st.st_ino;
#ifdef _WIN32
    out->isDirectory = (st.st_mode & _S_IFDIR) != 0;
#else
    out->isDirectory = S_ISDIR(st.st_mode);
#endif
    return true;
}

bool fileExists(const char* path) {
    FileInfo info;
    return statFile(path, &info);
}

bool deleteFile(const char* path) { return remove(path) == 0; }

bool readFileBytes(const char* path, uint8_t** outData, size_t* outLength) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    const long size = ftell(file);
    if (size < 0) { fclose(file); return false; }
    rewind(file);

    uint8_t* buffer = malloc(size > 0 ? (size_t)size : 1);
    if (buffer == NULL) { fclose(file); return false; }
    const size_t bytesRead = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (bytesRead != (size_t)size) { free(buffer); return false; }

    *outData = buffer;
    *outLength = (size_t)size;
    return true;
}

bool readFileRange(const char* path, uint64_t offset, uint32_t length, uint8_t* out) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
#ifdef _WIN32
    if (_fseeki64(file, (long long)offset, SEEK_SET) != 0) { fclose(file); return false; }
#else
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) { fclose(file); return false; }
#endif
    const size_t got = fread(out, 1, length, file);
    fclose(file);
    return got == length;
}

bool writeFileBytesAtomic(const char* path, const uint8_t* data, size_t length) {
    char tmpPath[SYNC_PATH_MAX];
    if (snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path) >= (int)sizeof(tmpPath)) return false;

    FILE* file = fopen(tmpPath, "wb");
    if (file == NULL) return false;
    const size_t written = length > 0 ? fwrite(data, 1, length, file) : 0;
    const bool flushed = fflush(file) == 0;
    fclose(file);
    if (written != length || !flushed) { remove(tmpPath); return false; }

    if (!platformRenameReplace(tmpPath, path)) { remove(tmpPath); return false; }
    return true;
}

bool fileInfoUnchanged(const FileInfo* before, const FileInfo* after) {
    return before->size == after->size && before->mtimeNs == after->mtimeNs && before->inode == after->inode;
}

GuardedReadStatus readFileGuarded(const char* path, uint8_t** outData, size_t* outLength, FileInfo* outInfo) {
    FileInfo before, after;
    if (!statFile(path, &before)) return GUARDED_READ_MISSING;
    if (before.isDirectory) return GUARDED_READ_ERROR;

    uint8_t* data = NULL;
    size_t length = 0;
    if (!readFileBytes(path, &data, &length)) {
        return fileExists(path) ? GUARDED_READ_ERROR : GUARDED_READ_MISSING;
    }
    if (!statFile(path, &after) || !fileInfoUnchanged(&before, &after) || length != before.size) {
        free(data);
        return GUARDED_READ_TAINTED;
    }

    *outData = data;
    *outLength = length;
    if (outInfo) *outInfo = after;
    return GUARDED_READ_OK;
}

bool mkdirRecursive(const char* path) {
    char buffer[SYNC_PATH_MAX];
    const size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, path, length + 1);

    for (size_t i = 1; i < length; i++) {
        if (buffer[i] == '/' || buffer[i] == '\\') {
            const char saved = buffer[i];
            buffer[i] = '\0';
            // Skip drive roots like "C:" and the empty component from a leading separator.
            const bool driveRoot = (i == 2 && buffer[1] == ':');
            if (!driveRoot && !platformMkdir(buffer)) return false;
            buffer[i] = saved;
        }
    }
    return platformMkdir(buffer);
}

bool pathJoin(char* out, size_t capacity, const char* a, const char* b) {
    const size_t aLength = strlen(a);
    const bool aEndsWithSeparator = aLength > 0 && (a[aLength - 1] == '/' || a[aLength - 1] == '\\');
    const int written = aEndsWithSeparator
        ? snprintf(out, capacity, "%s%s", a, b)
        : snprintf(out, capacity, "%s%c%s", a, PATH_SEPARATOR, b);
    return written >= 0 && (size_t)written < capacity;
}

static bool walkRecursive(const char* root, const char* relPrefix, WalkVisitor visit, void* userData) {
    char dirPath[SYNC_PATH_MAX];
    if (relPrefix[0] == '\0') {
        snprintf(dirPath, sizeof(dirPath), "%s", root);
    } else {
        char nativeRel[SYNC_PATH_MAX];
        relPathToNative(relPrefix, nativeRel, sizeof(nativeRel));
        if (!pathJoin(dirPath, sizeof(dirPath), root, nativeRel)) return false;
    }

    DIR* dir = opendir(dirPath);
    if (dir == NULL) return true; // unreadable directory: skip it rather than abort the whole scan

    bool keepGoing = true;
    const struct dirent* entry;
    while (keepGoing && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char relPath[SYNC_PATH_MAX];
        const int relWritten = relPrefix[0] == '\0'
            ? snprintf(relPath, sizeof(relPath), "%s", entry->d_name)
            : snprintf(relPath, sizeof(relPath), "%s/%s", relPrefix, entry->d_name);
        if (relWritten < 0 || (size_t)relWritten >= sizeof(relPath)) continue;

        char fullPath[SYNC_PATH_MAX];
        if (!pathJoin(fullPath, sizeof(fullPath), dirPath, entry->d_name)) continue;

        FileInfo info;
        if (!statFile(fullPath, &info)) continue; // vanished between readdir and stat

        keepGoing = visit(fullPath, relPath, &info, userData);
        if (keepGoing && info.isDirectory) {
            keepGoing = walkRecursive(root, relPath, visit, userData);
        }
    }
    closedir(dir);
    return keepGoing;
}

bool walkDirectory(const char* root, WalkVisitor visit, void* userData) {
    return walkRecursive(root, "", visit, userData);
}

bool isSafeRelativePath(const char* relPath) {
    const size_t length = strlen(relPath);
    if (length == 0 || length >= SYNC_PATH_MAX) return false;
    if (relPath[0] == '/' || relPath[0] == '\\') return false;
    if (length >= 2 && relPath[1] == ':') return false; // "C:..."
    if (strchr(relPath, '\\') != NULL) return false;    // wire paths are '/'-separated only

    const char* component = relPath;
    while (*component) {
        const char* end = strchr(component, '/');
        const size_t componentLength = end ? (size_t)(end - component) : strlen(component);
        if (componentLength == 0) return false;                                         // "a//b"
        if (componentLength == 2 && component[0] == '.' && component[1] == '.') return false;
        if (componentLength == 1 && component[0] == '.') return false;
        if (!end) break;
        component = end + 1;
    }
    return true;
}

void relPathToNative(const char* relPath, char* out, size_t capacity) {
    size_t i = 0;
    for (; relPath[i] != '\0' && i + 1 < capacity; i++) {
        out[i] = relPath[i] == '/' ? PATH_SEPARATOR : relPath[i];
    }
    out[i] = '\0';
}
