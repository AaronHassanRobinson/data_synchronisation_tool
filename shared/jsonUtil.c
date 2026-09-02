#define SJ_IMPL 1
#include "jsonUtil.h"
#include <string.h>
#include <stdlib.h>

bool jsonKeyIs(sj_Value key, const char* name) {
    const size_t length = (size_t)(key.end - key.start);
    return strlen(name) == length && memcmp(name, key.start, length) == 0;
}

bool jsonCopyString(sj_Value value, char* out, size_t capacity) {
    const size_t length = (size_t)(value.end - value.start);
    if (length + 1 > capacity) return false;
    memcpy(out, value.start, length);
    out[length] = '\0';
    return true;
}

bool jsonToUint32(sj_Value value, uint32_t* out) {
    char buffer[32];
    if (value.type != SJ_NUMBER && value.type != SJ_STRING) return false;
    if (!jsonCopyString(value, buffer, sizeof(buffer))) return false;
    char* end = NULL;
    const unsigned long parsed = strtoul(buffer, &end, 10);
    if (end == buffer || *end != '\0' || parsed > UINT32_MAX) return false;
    *out = (uint32_t)parsed;
    return true;
}

bool jsonToBool(sj_Value value, bool* out) {
    if (value.type != SJ_BOOL) return false;
    *out = value.start[0] == 't';
    return true;
}
