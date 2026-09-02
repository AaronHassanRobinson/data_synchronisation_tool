//
// String-keyed hash map (chaining). Used for the client state DB index, the polling watcher's
// snapshot, event de-duplication, and the server's root-label table. Values are caller-owned
// pointers; pass a free function to strMapDestroy if the map should release them.
//
#ifndef DATA_SYNCHRONISATION_TOOL_STRMAP_H
#define DATA_SYNCHRONISATION_TOOL_STRMAP_H
#include <stddef.h>
#include <stdint.h>

typedef struct StrMap StrMap;
typedef void (*StrMapFreeFn)(void* value);
typedef bool (*StrMapVisitor)(const char* key, void* value, void* userData); // return false to stop

StrMap* strMapCreate(void);
void strMapDestroy(StrMap* map, StrMapFreeFn freeValue);
// Inserts or replaces. Returns the previous value (NULL if none) so the caller can free it.
void* strMapPut(StrMap* map, const char* key, void* value);
void* strMapGet(const StrMap* map, const char* key);
bool strMapContains(const StrMap* map, const char* key);
void* strMapRemove(StrMap* map, const char* key); // returns the removed value or NULL
size_t strMapCount(const StrMap* map);
void strMapForEach(const StrMap* map, StrMapVisitor visit, void* userData);
uint64_t strHash64(const char* s);

#endif //DATA_SYNCHRONISATION_TOOL_STRMAP_H
