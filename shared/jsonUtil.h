//
// Small helpers over sj.h (the vendored JSON reader) so config parsing reads cleanly and every
// string copy is bounds-checked and NUL-terminated - sj values are slices, not C strings.
//
#ifndef DATA_SYNCHRONISATION_TOOL_JSON_UTIL_H
#define DATA_SYNCHRONISATION_TOOL_JSON_UTIL_H
#include <stddef.h>
#include <stdint.h>
#include "sj.h"

bool jsonKeyIs(sj_Value key, const char* name);
bool jsonCopyString(sj_Value value, char* out, size_t capacity);
bool jsonToUint32(sj_Value value, uint32_t* out);
bool jsonToBool(sj_Value value, bool* out);

#endif //DATA_SYNCHRONISATION_TOOL_JSON_UTIL_H
