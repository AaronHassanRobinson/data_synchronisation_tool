#include "fileUtil.h"
#include <stdio.h>
#include <stdlib.h>

bool readFileBytes(const char* path, uint8_t** outData, size_t* outLength) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);

    uint8_t* buffer = malloc(size > 0 ? (size_t)size : 1);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }

    const size_t bytesRead = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (bytesRead != (size_t)size) {
        free(buffer);
        return false;
    }

    *outData = buffer;
    *outLength = (size_t)size;
    return true;
}

bool writeFileBytesAtomic(const char* path, const uint8_t* data, size_t length) {
    char tmpPath[4096];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

    FILE* file = fopen(tmpPath, "wb");
    if (file == NULL) return false;

    const size_t written = fwrite(data, 1, length, file);
    fclose(file);
    if (written != length) {
        remove(tmpPath);
        return false;
    }

    if (rename(tmpPath, path) != 0) {
        remove(tmpPath);
        return false;
    }
    return true;
}
