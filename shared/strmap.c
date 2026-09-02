#include "strmap.h"
#include <stdlib.h>
#include <string.h>

typedef struct Entry {
    struct Entry* next;
    void* value;
    uint64_t hash;
    char key[];
} Entry;

struct StrMap {
    Entry** buckets;
    size_t bucketCount;
    size_t count;
};

uint64_t strHash64(const char* s) { // FNV-1a
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

StrMap* strMapCreate(void) {
    StrMap* map = calloc(1, sizeof(StrMap));
    map->bucketCount = 64;
    map->buckets = calloc(map->bucketCount, sizeof(Entry*));
    return map;
}

void strMapDestroy(StrMap* map, StrMapFreeFn freeValue) {
    if (!map) return;
    for (size_t i = 0; i < map->bucketCount; i++) {
        Entry* e = map->buckets[i];
        while (e) {
            Entry* next = e->next;
            if (freeValue) freeValue(e->value);
            free(e);
            e = next;
        }
    }
    free(map->buckets);
    free(map);
}

static void grow(StrMap* map) {
    const size_t newCount = map->bucketCount * 2;
    Entry** newBuckets = calloc(newCount, sizeof(Entry*));
    for (size_t i = 0; i < map->bucketCount; i++) {
        Entry* e = map->buckets[i];
        while (e) {
            Entry* next = e->next;
            const size_t slot = e->hash % newCount;
            e->next = newBuckets[slot];
            newBuckets[slot] = e;
            e = next;
        }
    }
    free(map->buckets);
    map->buckets = newBuckets;
    map->bucketCount = newCount;
}

static Entry* find(const StrMap* map, const char* key, uint64_t hash) {
    for (Entry* e = map->buckets[hash % map->bucketCount]; e; e = e->next) {
        if (e->hash == hash && strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

void* strMapPut(StrMap* map, const char* key, void* value) {
    const uint64_t hash = strHash64(key);
    Entry* existing = find(map, key, hash);
    if (existing) {
        void* previous = existing->value;
        existing->value = value;
        return previous;
    }
    if (map->count + 1 > map->bucketCount * 3 / 4) grow(map);

    const size_t keyLength = strlen(key);
    Entry* e = malloc(sizeof(Entry) + keyLength + 1);
    memcpy(e->key, key, keyLength + 1);
    e->value = value;
    e->hash = hash;
    const size_t slot = hash % map->bucketCount;
    e->next = map->buckets[slot];
    map->buckets[slot] = e;
    map->count++;
    return NULL;
}

void* strMapGet(const StrMap* map, const char* key) {
    const Entry* e = find(map, key, strHash64(key));
    return e ? e->value : NULL;
}

bool strMapContains(const StrMap* map, const char* key) {
    return find(map, key, strHash64(key)) != NULL;
}

void* strMapRemove(StrMap* map, const char* key) {
    const uint64_t hash = strHash64(key);
    Entry** link = &map->buckets[hash % map->bucketCount];
    while (*link) {
        Entry* e = *link;
        if (e->hash == hash && strcmp(e->key, key) == 0) {
            *link = e->next;
            void* value = e->value;
            free(e);
            map->count--;
            return value;
        }
        link = &e->next;
    }
    return NULL;
}

size_t strMapCount(const StrMap* map) { return map->count; }

void strMapForEach(const StrMap* map, StrMapVisitor visit, void* userData) {
    for (size_t i = 0; i < map->bucketCount; i++) {
        for (Entry* e = map->buckets[i]; e; ) {
            Entry* next = e->next; // visitor may remove the current entry
            if (!visit(e->key, e->value, userData)) return;
            e = next;
        }
    }
}
