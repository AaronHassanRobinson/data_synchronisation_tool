//
// Unit tests. Zero-dependency: a CHECK macro, a counter, and a temp directory under the cwd.
// Run via `ctest` or directly as `sync_tests`.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sha256.h"
#include "hmac.h"
#include "cdc.h"
#include "cdcProtocol.h"
#include "compress.h"
#include "fileUtil.h"
#include "protocol.h"
#include "strmap.h"
#include "platform.h"
#include "config.h"
#include "db.h"
#include "eventQueue.h"
#include "scanner.h"
#include "session.h"
#include "chunkStore.h"
#include "serverConfig.h"
#include "serverSession.h"

#ifndef _WIN32
#include <sys/socket.h>
#endif

static int checksRun = 0, checksFailed = 0;
#define CHECK(condition) do { checksRun++; if (!(condition)) { checksFailed++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define CHECK_EQ_STR(a, b) CHECK(strcmp((a), (b)) == 0)

static char tempRoot[SYNC_PATH_MAX];
static void tempPath(const char* name, char* out) { pathJoin(out, SYNC_PATH_MAX, tempRoot, name); }
static void writeText(const char* path, const char* text) { writeFileBytesAtomic(path, (const uint8_t*)text, strlen(text)); }
static void removeTree(const char* path) {
    char command[SYNC_PATH_MAX + 32];
#ifdef _WIN32
    snprintf(command, sizeof(command), "rmdir /s /q \"%s\"", path);
#else
    snprintf(command, sizeof(command), "rm -rf \"%s\"", path);
#endif
    (void)system(command);
}

static bool hexEquals(const uint8_t* digest, const char* expectedHex) {
    char hex[65];
    sha256ToHex(digest, hex);
    return strcmp(hex, expectedHex) == 0;
}

// Deterministic pseudo-random bytes so the CDC tests have realistic (incompressible) content.
static void fillRandom(uint8_t* out, size_t length, uint64_t seed) {
    for (size_t i = 0; i < length; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = (uint8_t)(seed >> 56);
    }
}

// ---------------------------------------------------------------- hashing

static void testSha256(void) {
    printf("sha256\n");
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256Buffer("", 0, digest);
    CHECK(hexEquals(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256Buffer("abc", 3, digest);
    CHECK(hexEquals(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char* twoBlock = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256Buffer(twoBlock, strlen(twoBlock), digest);
    CHECK(hexEquals(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // Incremental feeding in odd sizes must equal one-shot hashing.
    uint8_t data[1000];
    fillRandom(data, sizeof(data), 7);
    uint8_t oneShot[SHA256_DIGEST_SIZE], incremental[SHA256_DIGEST_SIZE];
    sha256Buffer(data, sizeof(data), oneShot);
    Sha256Context ctx;
    sha256Init(&ctx);
    for (size_t offset = 0, step = 1; offset < sizeof(data); offset += step, step = step % 97 + 1) {
        const size_t n = offset + step > sizeof(data) ? sizeof(data) - offset : step;
        sha256Update(&ctx, data + offset, n);
    }
    sha256Final(&ctx, incremental);
    CHECK(memcmp(oneShot, incremental, SHA256_DIGEST_SIZE) == 0);
}

static void testHmac(void) {
    printf("hmac-sha256\n");
    uint8_t mac[SHA256_DIGEST_SIZE];
    // RFC 4231 test case 2
    hmacSha256((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, mac);
    CHECK(hexEquals(mac, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    // RFC 4231 test case 1
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    hmacSha256(key, sizeof(key), (const uint8_t*)"Hi There", 8, mac);
    CHECK(hexEquals(mac, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    // RFC 4231 test case 6: key longer than a block
    uint8_t longKey[131];
    memset(longKey, 0xaa, sizeof(longKey));
    const char* message = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmacSha256(longKey, sizeof(longKey), (const uint8_t*)message, strlen(message), mac);
    CHECK(hexEquals(mac, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

    const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};
    CHECK(constantTimeEquals(a, a, 4));
    CHECK(!constantTimeEquals(a, b, 4));

    uint8_t r1[32], r2[32];
    CHECK(randomBytes(r1, 32) && randomBytes(r2, 32) && memcmp(r1, r2, 32) != 0);
}

// ---------------------------------------------------------------- CDC

static void testCdc(void) {
    printf("cdc\n");
    const CdcParams params = { 64, 1024, 7 };
    CHECK(cdcParamsValid(&params));
    const CdcParams bad = { 8, 4, 2 };
    CHECK(!cdcParamsValid(&bad));

    enum { SIZE = 40000 };
    uint8_t* data = malloc(SIZE);
    fillRandom(data, SIZE, 42);

    CdcChunkSet set = cdcChunkBuffer(data, SIZE, &params);
    CHECK(set.chunkCount > 10);
    CHECK(set.totalLength == SIZE);

    // Chunks tile the buffer exactly, respect the size bounds, and hash to their content.
    uint64_t expectedOffset = 0;
    bool boundsOk = true, hashesOk = true;
    for (uint32_t i = 0; i < set.chunkCount; i++) {
        const CdcChunkDescriptor* c = &set.chunks[i];
        if (c->offset != expectedOffset) boundsOk = false;
        if (c->length > params.maxChunkSize) boundsOk = false;
        if (i + 1 < set.chunkCount && c->length < params.minChunkSize) boundsOk = false;
        uint8_t h[SHA256_DIGEST_SIZE];
        sha256Buffer(data + c->offset, c->length, h);
        if (memcmp(h, c->hash, SHA256_DIGEST_SIZE) != 0) hashesOk = false;
        expectedOffset += c->length;
    }
    CHECK(boundsOk);
    CHECK(hashesOk);
    CHECK(expectedOffset == SIZE);
    uint8_t whole[SHA256_DIGEST_SIZE];
    sha256Buffer(data, SIZE, whole);
    CHECK(memcmp(whole, set.fileHash, SHA256_DIGEST_SIZE) == 0);

    // Deterministic.
    CdcChunkSet again = cdcChunkBuffer(data, SIZE, &params);
    CHECK(again.chunkCount == set.chunkCount && memcmp(again.chunks, set.chunks, set.chunkCount * sizeof(CdcChunkDescriptor)) == 0);
    cdcFreeChunkSet(&again);

    // Streaming from a file, in 1 MiB reads, equals in-memory chunking.
    char path[SYNC_PATH_MAX];
    tempPath("cdc.bin", path);
    CHECK(writeFileBytesAtomic(path, data, SIZE));
    CdcChunkSet fromFile;
    CHECK(cdcChunkFile(path, &params, &fromFile));
    CHECK(fromFile.chunkCount == set.chunkCount && memcmp(fromFile.chunks, set.chunks, set.chunkCount * sizeof(CdcChunkDescriptor)) == 0);
    cdcFreeChunkSet(&fromFile);

    // The point of CDC: inserting bytes in the middle leaves most chunk hashes intact.
    uint8_t* edited = malloc(SIZE + 100);
    memcpy(edited, data, SIZE / 2);
    memset(edited + SIZE / 2, 'X', 100);
    memcpy(edited + SIZE / 2 + 100, data + SIZE / 2, SIZE - SIZE / 2);
    CdcChunkSet editedSet = cdcChunkBuffer(edited, SIZE + 100, &params);
    uint32_t shared = 0;
    for (uint32_t i = 0; i < editedSet.chunkCount; i++) {
        for (uint32_t j = 0; j < set.chunkCount; j++) {
            if (memcmp(editedSet.chunks[i].hash, set.chunks[j].hash, SHA256_DIGEST_SIZE) == 0) { shared++; break; }
        }
    }
    printf("  insertion test: %u of %u chunks unchanged after a mid-file insert\n", shared, editedSet.chunkCount);
    CHECK(shared + 3 >= editedSet.chunkCount); // at most the chunk(s) around the edit changed
    CHECK(shared >= set.chunkCount / 2);
    cdcFreeChunkSet(&editedSet);
    free(edited);

    // Empty input: no chunks, hash of the empty string.
    CdcChunkSet empty = cdcChunkBuffer(data, 0, &params);
    CHECK(empty.chunkCount == 0 && empty.totalLength == 0);
    CHECK(hexEquals(empty.fileHash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    cdcFreeChunkSet(&empty);

    // Max-size cut: a run of identical bytes never hits a boundary, so chunks are exactly max.
    memset(data, 0, SIZE);
    CdcChunkSet zeros = cdcChunkBuffer(data, SIZE, &params);
    CHECK(zeros.chunkCount == (SIZE + params.maxChunkSize - 1) / params.maxChunkSize);
    CHECK(zeros.chunks[0].length == params.maxChunkSize);
    cdcFreeChunkSet(&zeros);

    cdcFreeChunkSet(&set);
    free(data);
}

// ---------------------------------------------------------------- payloads

static bool countRoots(uint32_t index, const char* label, void* userData) {
    const char** expected = userData;
    CHECK(strcmp(label, expected[index]) == 0);
    return true;
}

static void testPayloads(void) {
    printf("cdcProtocol payloads\n");
    uint32_t length = 0;
    const CdcParams cdc = { 64, 2048, 8 };

    void* hello = buildHelloPayload("client-x", CAP_ZSTD, &cdc, &length);
    HelloHeader helloHeader;
    char clientId[PROTOCOL_MAX_CLIENT_ID + 1];
    CHECK(parseHelloPayload(hello, length, &helloHeader, clientId, sizeof(clientId)));
    CHECK_EQ_STR(clientId, "client-x");
    CHECK(helloHeader.capabilities == CAP_ZSTD && helloHeader.cdc.maskBits == 8);
    CHECK(!parseHelloPayload(hello, length - 1, &helloHeader, clientId, sizeof(clientId))); // truncated
    free(hello);

    const char* labels[] = { "documents", "logs" };
    void* roots = buildWatchRootsPayload(labels, 2, &length);
    CHECK(parseWatchRootsPayload(roots, length, countRoots, labels));
    CHECK(!parseWatchRootsPayload(roots, length - 2, countRoots, labels));
    free(roots);

    void* dirMeta = buildDirMetaPayload(1, "a/b/c", 1234, &length);
    DirMetaHeader dirHeader;
    char relPath[SYNC_PATH_MAX];
    CHECK(parseDirMetaPayload(dirMeta, length, &dirHeader, relPath, sizeof(relPath)));
    CHECK(dirHeader.rootIndex == 1 && dirHeader.mtimeSeconds == 1234);
    CHECK_EQ_STR(relPath, "a/b/c");
    free(dirMeta);

    uint8_t data[5000];
    fillRandom(data, sizeof(data), 3);
    CdcChunkSet set = cdcChunkBuffer(data, sizeof(data), &cdc);
    void* manifest = buildManifestPayload(0, "dir/file.bin", 99, &set, &length);
    FileManifestHeader manifestHeader;
    const CdcChunkDescriptor* descriptors = NULL;
    CHECK(parseManifestPayload(manifest, length, &manifestHeader, relPath, sizeof(relPath), &descriptors));
    CHECK(manifestHeader.chunkCount == set.chunkCount && manifestHeader.fileSize == sizeof(data));
    CHECK(memcmp(descriptors, set.chunks, set.chunkCount * sizeof(CdcChunkDescriptor)) == 0);
    CHECK_EQ_STR(relPath, "dir/file.bin");
    // A manifest whose chunks don't tile the file is rejected.
    ((FileManifestHeader*)manifest)->fileSize += 1;
    CHECK(!parseManifestPayload(manifest, length, &manifestHeader, relPath, sizeof(relPath), &descriptors));
    free(manifest);

    const uint32_t indices[] = { 3, 1, 4 };
    void* needed = buildNeededPayload(indices, 3, &length);
    const uint32_t* parsedIndices = NULL;
    uint32_t count = 0;
    CHECK(parseNeededPayload(needed, length, &parsedIndices, &count));
    CHECK(count == 3 && parsedIndices[0] == 3 && parsedIndices[2] == 4);
    CHECK(!parseNeededPayload(needed, length - 1, &parsedIndices, &count));
    free(needed);

    void* chunk = buildChunkDataPayload(7, data, 100, &length);
    uint32_t chunkIndex = 0, dataLength = 0;
    const uint8_t* chunkData = NULL;
    CHECK(parseChunkDataPayload(chunk, length, &chunkIndex, &chunkData, &dataLength));
    CHECK(chunkIndex == 7 && dataLength == 100 && memcmp(chunkData, data, 100) == 0);
    CHECK(!parseChunkDataPayload(chunk, length + 1, &chunkIndex, &chunkData, &dataLength));
    free(chunk);
    cdcFreeChunkSet(&set);
}

// ---------------------------------------------------------------- framing + compression

static void testFraming(void) {
    printf("protocol framing (compression %s)\n", compressionAvailable() ? "available" : "unavailable");
    if (compressionAvailable()) {
        uint8_t text[4096];
        for (size_t i = 0; i < sizeof(text); i++) text[i] = "hello world "[i % 12];
        size_t compressedLength = 0;
        uint8_t* compressed = compressBuffer(text, sizeof(text), &compressedLength);
        CHECK(compressed && compressedLength < sizeof(text) / 4);
        uint8_t* restored = decompressBuffer(compressed, compressedLength, sizeof(text));
        CHECK(restored && memcmp(restored, text, sizeof(text)) == 0);
        CHECK(decompressBuffer(compressed, compressedLength, sizeof(text) - 1) == NULL); // wrong expected size
        free(compressed);
        free(restored);
    }
#ifndef _WIN32
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    ProtocolLink a = { transportPlain(pair[0]), compressionAvailable() };
    ProtocolLink b = { transportPlain(pair[1]), compressionAvailable() };

    uint8_t big[200000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i % 7); // compressible
    CHECK(sendMessage(&a, MSG_CHUNK_DATA, big, sizeof(big)));
    CHECK(sendMessage(&a, MSG_BYE, NULL, 0));

    MessageType type;
    void* payload = NULL;
    uint32_t length = 0;
    CHECK(recvMessage(&b, &type, &payload, &length));
    CHECK(type == MSG_CHUNK_DATA && length == sizeof(big) && memcmp(payload, big, sizeof(big)) == 0);
    free(payload);
    CHECK(recvMessage(&b, &type, &payload, &length));
    CHECK(type == MSG_BYE && length == 0);
    free(payload);

    // Garbage on the wire (bad magic) is rejected rather than interpreted.
    const uint8_t garbage[sizeof(ProtocolHeader)] = { 0xde, 0xad };
    CHECK(transportSend(a.transport, garbage, sizeof(garbage)));
    CHECK(!recvMessage(&b, &type, &payload, &length));

    transportClose(a.transport);
    transportClose(b.transport);
#endif
}

// ---------------------------------------------------------------- files / paths

static void testFileUtil(void) {
    printf("fileUtil\n");
    CHECK(isSafeRelativePath("a/b/c.txt"));
    CHECK(isSafeRelativePath("file"));
    CHECK(!isSafeRelativePath(""));
    CHECK(!isSafeRelativePath("/etc/passwd"));
    CHECK(!isSafeRelativePath("../x"));
    CHECK(!isSafeRelativePath("a/../../x"));
    CHECK(!isSafeRelativePath("a/./b"));
    CHECK(!isSafeRelativePath("a//b"));
    CHECK(!isSafeRelativePath("C:/x"));
    CHECK(!isSafeRelativePath("a\\b"));

    char nested[SYNC_PATH_MAX];
    tempPath("deep/er/dir", nested);
    CHECK(mkdirRecursive(nested));
    FileInfo info;
    CHECK(statFile(nested, &info) && info.isDirectory);
    CHECK(mkdirRecursive(nested)); // idempotent

    char path[SYNC_PATH_MAX];
    tempPath("deep/er/dir/data.bin", path);
    uint8_t data[3000];
    fillRandom(data, sizeof(data), 9);
    CHECK(writeFileBytesAtomic(path, data, sizeof(data)));
    uint8_t* read = NULL;
    size_t length = 0;
    CHECK(readFileBytes(path, &read, &length) && length == sizeof(data) && memcmp(read, data, length) == 0);
    free(read);
    uint8_t range[100];
    CHECK(readFileRange(path, 1000, 100, range) && memcmp(range, data + 1000, 100) == 0);
    CHECK(!readFileRange(path, 2950, 100, range)); // past EOF

    FileInfo guarded;
    CHECK(readFileGuarded(path, &read, &length, &guarded) == GUARDED_READ_OK && length == sizeof(data));
    free(read);
    CHECK(guarded.size == sizeof(data));
    char missing[SYNC_PATH_MAX];
    tempPath("nope", missing);
    CHECK(readFileGuarded(missing, &read, &length, &guarded) == GUARDED_READ_MISSING);

    FileInfo before = { .size = 10, .mtimeNs = 5, .inode = 1 }, after = before;
    CHECK(fileInfoUnchanged(&before, &after));
    after.mtimeNs++;
    CHECK(!fileInfoUnchanged(&before, &after));

    char joined[64];
    CHECK(pathJoin(joined, sizeof(joined), "a/", "b") && strcmp(joined, "a/b") == 0);
    CHECK(!pathJoin(joined, 4, "abc", "def"));

}

static bool walkCounter(const char* fullPath, const char* relPath, const FileInfo* info, void* userData) {
    (void)fullPath;
    int* counts = userData; // [files, dirs, sawNested]
    if (info->isDirectory) counts[1]++; else counts[0]++;
    if (strcmp(relPath, "deep/er/dir/data.bin") == 0) counts[2] = 1;
    return true;
}

static void testWalk(void) {
    printf("walkDirectory\n");
    int counts[3] = {0, 0, 0};
    CHECK(walkDirectory(tempRoot, walkCounter, counts));
    CHECK(counts[0] >= 2);   // cdc.bin + data.bin (+ whatever else earlier tests wrote)
    CHECK(counts[1] >= 3);   // deep, er, dir
    CHECK(counts[2] == 1);
}

// ---------------------------------------------------------------- strmap

static void testStrMap(void) {
    printf("strmap\n");
    StrMap* map = strMapCreate();
    char key[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "key-%d", i);
        CHECK(strMapPut(map, key, (void*)(intptr_t)(i + 1)) == NULL);
    }
    CHECK(strMapCount(map) == 1000);
    CHECK(strMapGet(map, "key-500") == (void*)(intptr_t)501);
    CHECK(strMapPut(map, "key-500", (void*)(intptr_t)7) == (void*)(intptr_t)501);
    CHECK(strMapRemove(map, "key-500") == (void*)(intptr_t)7);
    CHECK(!strMapContains(map, "key-500") && strMapCount(map) == 999);
    CHECK(strMapRemove(map, "absent") == NULL);
    strMapDestroy(map, NULL);
}

// ---------------------------------------------------------------- config

static void testConfig(void) {
    printf("config\n");
    const char* json =
        "{ \"server_ip_address\": \"10.0.0.5\", \"server_port\": 9100, \"client_id\": \"c1\","
        "  \"pre_shared_key\": \"secret\", \"use_tls\": false, \"use_compression\": false,"
        "  \"directory_paths\": [\"/data/a/\", \"/data/b\"], \"database_path\": \"x.db\","
        "  \"watcher_backend\": \"poll\", \"scan_interval_seconds\": 60, \"batch_max_events\": 7,"
        "  \"batch_window_seconds\": 2, \"cdc_min_chunk_size\": 32, \"cdc_max_chunk_size\": 4096,"
        "  \"cdc_mask_bits\": 9, \"unknown_future_key\": 1 }";
    ClientConfig config;
    char error[256] = "";
    CHECK(clientConfigParse(json, strlen(json), &config, error, sizeof(error)));
    CHECK_EQ_STR(config.serverIp, "10.0.0.5");
    CHECK(config.serverPort == 9100 && !config.useTls && !config.useCompression);
    CHECK(config.rootCount == 2);
    CHECK_EQ_STR(config.rootPaths[0], "/data/a"); // trailing slash stripped
    CHECK(config.watcherBackend == WATCHER_BACKEND_POLL && config.batchMaxEvents == 7 && config.cdc.maskBits == 9);
    CHECK(config.reconnectDelaySeconds == 5); // default survived

    const char* legacy = "{ \"pre_shared_key\": \"k\", \"directory_paths\": \"just/one\" }";
    CHECK(clientConfigParse(legacy, strlen(legacy), &config, error, sizeof(error)) && config.rootCount == 1);

    const char* noKey = "{ \"directory_paths\": [\"x\"] }";
    CHECK(!clientConfigParse(noKey, strlen(noKey), &config, error, sizeof(error)));
    CHECK(strstr(error, "pre_shared_key") != NULL);

    const char* badHash = "{ \"pre_shared_key\": \"k\", \"directory_paths\": [\"x\"], \"hash_algorithm\": \"md5\" }";
    CHECK(!clientConfigParse(badHash, strlen(badHash), &config, error, sizeof(error)));

    const char* badPort = "{ \"pre_shared_key\": \"k\", \"directory_paths\": [\"x\"], \"server_port\": 70000 }";
    CHECK(!clientConfigParse(badPort, strlen(badPort), &config, error, sizeof(error)));

    const char* serverJson = "{ \"listen_port\": 9, \"pre_shared_key\": \"k\", \"allowed_client_ids\": [\"a\", \"b\"] }";
    ServerConfig server;
    CHECK(serverConfigParse(serverJson, strlen(serverJson), &server, error, sizeof(error)));
    CHECK(server.listenPort == 9 && server.allowedClientCount == 2);
    CHECK(serverConfigClientAllowed(&server, "b") && !serverConfigClientAllowed(&server, "c"));
    server.allowedClientCount = 0;
    CHECK(serverConfigClientAllowed(&server, "anyone"));
}

// ---------------------------------------------------------------- db

static void testDb(void) {
    printf("db\n");
    char path[SYNC_PATH_MAX];
    tempPath("state.db", path);
    StateDb* db = dbOpen(path);
    CHECK(dbFileCount(db) == 0);

    CdcChunkDescriptor chunks[2] = { { 0, 100, {1} }, { 100, 50, {2} } };
    DbFileRecord record = { .rootIndex = 1, .size = 150, .mtimeNs = 123456789, .inode = 42, .chunkCount = 2, .chunks = chunks };
    snprintf(record.relPath, sizeof(record.relPath), "dir with space/file.txt");
    memset(record.fileHash, 0xab, SHA256_DIGEST_SIZE);
    dbUpsertFile(db, &record);
    dbAddDirectory(db, 1, "dir with space");
    dbAddDirectory(db, 0, "other");
    CHECK(dbSave(db));
    dbClose(db);

    db = dbOpen(path);
    CHECK(dbFileCount(db) == 1 && dbDirectoryCount(db) == 2);
    const DbFileRecord* loaded = dbFindFile(db, 1, "dir with space/file.txt");
    CHECK(loaded != NULL);
    if (loaded) {
        CHECK(loaded->size == 150 && loaded->mtimeNs == 123456789 && loaded->inode == 42);
        CHECK(loaded->chunkCount == 2 && loaded->chunks[1].offset == 100 && loaded->chunks[1].hash[0] == 2);
        CHECK(loaded->fileHash[31] == 0xab);
        const FileInfo same = { .size = 150, .mtimeNs = 123456789, .inode = 42 };
        const FileInfo touched = { .size = 150, .mtimeNs = 999, .inode = 42 };
        CHECK(dbRecordMatchesStat(loaded, &same));
        CHECK(!dbRecordMatchesStat(loaded, &touched));
    }
    CHECK(dbHasDirectory(db, 1, "dir with space") && dbHasDirectory(db, 0, "other") && !dbHasDirectory(db, 1, "other"));
    CHECK(dbFindFile(db, 0, "dir with space/file.txt") == NULL); // different root
    CHECK(dbRemoveFile(db, 1, "dir with space/file.txt") && dbFileCount(db) == 0);
    dbClose(db);
}

// ---------------------------------------------------------------- event queue + batching

static void testEventQueue(void) {
    printf("eventQueue + batching\n");
    EventQueue* queue = eventQueueCreate();
    CHECK(eventQueuePush(queue, EVENT_FILE_CHANGED, 0, "a.txt"));
    CHECK(!eventQueuePush(queue, EVENT_FILE_CHANGED, 0, "a.txt")); // duplicate collapsed
    CHECK(eventQueuePush(queue, EVENT_FILE_CHANGED, 1, "a.txt"));  // different root: distinct
    CHECK(eventQueuePush(queue, EVENT_DIR_CREATED, 0, "d"));
    CHECK(eventQueuePush(queue, EVENT_OVERFLOW, 0, NULL));
    CHECK(eventQueueCount(queue) == 4);

    SyncEvent event;
    CHECK(eventQueuePop(queue, &event, 10) && event.kind == EVENT_FILE_CHANGED && event.rootIndex == 0);
    CHECK(eventQueuePush(queue, EVENT_FILE_CHANGED, 0, "a.txt")); // allowed again once popped
    CHECK(eventQueuePop(queue, &event, 10) && event.rootIndex == 1);
    CHECK(eventQueuePop(queue, &event, 10) && event.kind == EVENT_DIR_CREATED);
    CHECK(eventQueuePop(queue, &event, 10) && event.kind == EVENT_OVERFLOW);
    CHECK(eventQueuePop(queue, &event, 10) && strcmp(event.relPath, "a.txt") == 0);
    const uint64_t start = platformMonotonicMs();
    CHECK(!eventQueuePop(queue, &event, 100));
    CHECK(platformMonotonicMs() - start >= 90);
    eventQueueDestroy(queue);

    CHECK(!batchShouldFlush(0, 0, 100000, 10, 5));         // nothing pending
    CHECK(batchShouldFlush(10, 100000, 100000, 10, 5));    // hit max events
    CHECK(!batchShouldFlush(3, 100000, 101000, 10, 5));    // 1s since last event, window 5s
    CHECK(batchShouldFlush(3, 100000, 105000, 10, 5));     // window elapsed
    CHECK(batchShouldFlush(1, 100000, 100000, 1, 5));      // max events of 1 = flush immediately
}

// ---------------------------------------------------------------- scanner

static void testScanner(void) {
    printf("scanner\n");
    char root[SYNC_PATH_MAX], sub[SYNC_PATH_MAX], fileA[SYNC_PATH_MAX], fileB[SYNC_PATH_MAX];
    tempPath("scanroot", root);
    tempPath("scanroot/sub", sub);
    tempPath("scanroot/a.txt", fileA);
    tempPath("scanroot/sub/b.txt", fileB);
    mkdirRecursive(sub);
    writeText(fileA, "aaa");
    writeText(fileB, "bbb");

    ClientConfig config;
    clientConfigDefaults(&config);
    snprintf(config.rootPaths[0], SYNC_PATH_MAX, "%s", root);
    config.rootCount = 1;
    char dbFile[SYNC_PATH_MAX];
    tempPath("scan.db", dbFile);
    StateDb* db = dbOpen(dbFile);
    EventQueue* queue = eventQueueCreate();

    ScanStats stats = scannerFullScan(&config, db, queue);
    CHECK(stats.filesSeen == 2 && stats.filesQueued == 2 && stats.directoriesSeen == 1 && stats.directoriesQueued == 1);
    CHECK(eventQueueCount(queue) == 3);
    SyncEvent event;
    while (eventQueuePop(queue, &event, 0)) {}

    // Record a.txt and sub as synced with their real stat triple: the next scan skips them.
    FileInfo info;
    CHECK(statFile(fileA, &info));
    DbFileRecord record = { .rootIndex = 0, .size = info.size, .mtimeNs = info.mtimeNs, .inode = info.inode };
    snprintf(record.relPath, sizeof(record.relPath), "a.txt");
    dbUpsertFile(db, &record);
    dbAddDirectory(db, 0, "sub");
    stats = scannerFullScan(&config, db, queue);
    CHECK(stats.filesQueued == 1 && stats.directoriesQueued == 0);
    CHECK(eventQueuePop(queue, &event, 0) && strcmp(event.relPath, "sub/b.txt") == 0);

    // A modified file (different size) is picked up again.
    writeText(fileA, "aaaa");
    stats = scannerFullScan(&config, db, queue);
    CHECK(stats.filesQueued == 2);

    eventQueueDestroy(queue);
    dbClose(db);
}

// ---------------------------------------------------------------- server pieces

static void testChunkStore(void) {
    printf("chunkStore\n");
    char dir[SYNC_PATH_MAX];
    tempPath("chunks", dir);
    ChunkStore* store = chunkStoreOpen(dir);
    CHECK(store != NULL);
    uint8_t data[500];
    fillRandom(data, sizeof(data), 11);
    uint8_t hash[SHA256_DIGEST_SIZE];
    sha256Buffer(data, sizeof(data), hash);

    CHECK(!chunkStoreHas(store, hash));
    uint8_t wrongHash[SHA256_DIGEST_SIZE];
    memset(wrongHash, 0x11, sizeof(wrongHash));
    CHECK(!chunkStorePut(store, wrongHash, data, sizeof(data))); // integrity check on the way in
    CHECK(!chunkStoreHas(store, wrongHash));
    CHECK(chunkStorePut(store, hash, data, sizeof(data)));
    CHECK(chunkStoreHas(store, hash));
    CHECK(chunkStorePut(store, hash, data, sizeof(data))); // idempotent
    uint8_t out[500];
    CHECK(chunkStoreRead(store, hash, out, sizeof(out)) && memcmp(out, data, sizeof(out)) == 0);
    CHECK(!chunkStoreRead(store, hash, out, 499)); // length must match exactly
    CHECK(!chunkStoreRead(store, wrongHash, out, 500));
    chunkStoreClose(store);
}

static void testLabels(void) {
    printf("root labels\n");
    char label[256];
    sessionRootLabel("/home/me/documents/", label, sizeof(label));
    CHECK_EQ_STR(label, "documents");
    sessionRootLabel("client/roots/logs", label, sizeof(label));
    CHECK_EQ_STR(label, "logs");
    sessionRootLabel("/", label, sizeof(label));
    CHECK_EQ_STR(label, "root");
    sessionRootLabel("C:\\data\\stuff", label, sizeof(label));
    CHECK_EQ_STR(label, "stuff");

    serverSanitizeLabel("my docs/../etc", label, sizeof(label));
    CHECK_EQ_STR(label, "my_docs_.._etc");
    CHECK(isSafeRelativePath(label));
    serverSanitizeLabel("..", label, sizeof(label));
    CHECK_EQ_STR(label, "root");
    serverSanitizeLabel("", label, sizeof(label));
    CHECK_EQ_STR(label, "root");
}

int main(void) {
    snprintf(tempRoot, sizeof(tempRoot), "sync_unit_tmp_%llu", (unsigned long long)platformWallClockSeconds());
    removeTree(tempRoot);
    if (!mkdirRecursive(tempRoot)) { fprintf(stderr, "cannot create temp dir\n"); return 1; }
    platformNetInit();

    testSha256();
    testHmac();
    testCdc();
    testPayloads();
    testFraming();
    testFileUtil();
    testWalk();
    testStrMap();
    testConfig();
    testDb();
    testEventQueue();
    testScanner();
    testChunkStore();
    testLabels();

    platformNetShutdown();
    removeTree(tempRoot);
    printf("\n%d checks, %d failed\n", checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}
