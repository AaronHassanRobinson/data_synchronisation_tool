# Data synchronisation tool

A client/server tool that continuously replicates a set of directories from a client machine to
a remote *collection* server: the server only ever accumulates verified data, and only the bytes
that actually changed cross the wire. It implements the design document for the project
(custom transfer instead of rsync, content-defined chunking, batched change detection, two-tier
integrity verification), in C, for Linux, macOS and Windows.

![data_synchronisation_tool.drawio.png](docs/data_synchronisation_tool.drawio.png)

**Status.** Everything in the design is implemented and exercised by the test suite except the
transport choice: the design calls for QUIC via MsQuic, which is not installable from the package
managers of either target platform, so the shipped transport is TCP + TLS behind a transport
interface that a QUIC backend would slot into (see [Deviations](#deviations-from-the-design-and-known-limitations)).
Builds and runs on macOS and Linux (both tested); the Windows build cross-compiles cleanly with
mingw-w64 but has not been executed on a Windows host.

## Building

Requirements: CMake >= 3.20, a C23-capable compiler (GCC 13+, Clang 16+), OpenSSL 3 (TLS) and
zstd (compression). The last two are optional - the build warns and continues without them, and
the protocol simply never negotiates the missing feature - but you want both.

```sh
# macOS
brew install cmake openssl@3 zstd
# Ubuntu / Debian
sudo apt install cmake gcc libssl-dev libzstd-dev

cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure      # unit + integration tests
```

Binaries land in `build/client/sync_client`, `build/server/sync_server` and `build/tests/sync_tests`.

Windows: cross-compile from Linux with mingw-w64 (`-DCMAKE_TOOLCHAIN_FILE=` pointing at
`x86_64-w64-mingw32-gcc`), or build natively with MSYS2/mingw and its `openssl`/`zstd` packages.

## Quick start

```sh
# 1. a self-signed certificate for the server; prints the fingerprint the client pins
deploy/gen-server-cert.sh server/certs

# 2. put the fingerprint in client/clientConfig.json -> "server_certificate_sha256", and pick the
#    same "pre_shared_key" in client/clientConfig.json and server/serverConfig.json

# 3. run the server (terminal 1) and the client (terminal 2), from the repo root
./build/server/sync_server server/serverConfig.json
./build/client/sync_client client/clientConfig.json          # daemon: watch + sync forever
./build/client/sync_client client/clientConfig.json --once   # one pass: scan, sync, exit
```

The sample config watches `client/roots/documents` and `client/roots/logs`; the server writes
them to `server_data/documents` and `server_data/logs`. Edit a file under `client/roots/` and
watch the client log: a few seconds later it reports something like

```
batch: flushing 1 event(s)
  file report.txt: 8947 bytes, 6 chunks, sent 2 (1104 bytes, 0 resent), server already had 4
batch: done - 1 synced, 0 unchanged, 0 skipped; 1104 of 8947 bytes sent
```

Insert a line in the *middle* of a file to see the point of content-defined chunking: every byte
after the edit shifts, yet only the chunk containing the edit is re-sent.

## Configuration

Both sides read a human-editable JSON file at startup (`sync_client [path]`, `sync_server [path]`;
the defaults are `client/clientConfig.json` and `server/serverConfig.json`). A restart picks up
changes - which the deploy wrappers below make automatic.

### client/clientConfig.json

| key | default | meaning |
|---|---|---|
| `server_ip_address`, `server_port` | `127.0.0.1`, `9001` | where the collection server listens |
| `client_id` | `client-1` | identity presented during authentication |
| `pre_shared_key` | *(required)* | shared secret for the mutual HMAC challenge/response |
| `use_tls` | `true` | encrypt the transport; refuses to run in plaintext if the build lacks OpenSSL |
| `server_certificate_sha256` | `""` | pin of the server's DER certificate (hex). Empty = accept any, with a warning |
| `use_compression` | `true` | offer zstd; used only if both sides have it |
| `directory_paths` | *(required)* | array of root directories to watch recursively |
| `database_path` | `sync-state.db` | on-disk record of the last server-confirmed state |
| `watcher_backend` | `native` | `native` = inotify / ReadDirectoryChangesW, `poll` = portable stat-walk |
| `scan_interval_seconds` | `900` | periodic full recursive scan |
| `batch_max_events` / `batch_window_seconds` | `100` / `5` | sync after this many distinct events, or this long after the last one |
| `poll_interval_seconds` | `2` | cadence of the polling watcher |
| `reconnect_delay_seconds` | `5` | back-off between connection attempts |
| `socket_timeout_ms` | `30000` | send/receive timeout so a dead peer surfaces as an error |
| `chunk_retry_limit` | `3` | rounds of re-sending chunks the server nacked |
| `cdc_min_chunk_size` / `cdc_max_chunk_size` / `cdc_mask_bits` | `16384` / `262144` / `16` | chunk bounds and target average (2^bits) |
| `hash_algorithm` | `sha256` | chunk/file hash; only `sha256` is implemented, anything else is rejected |

### server/serverConfig.json

| key | default | meaning |
|---|---|---|
| `listen_port` | `9001` | |
| `output_directory` | `server_data` | one sub-directory per watched root, plus `.chunks/` |
| `pre_shared_key` | *(required)* | must match the client |
| `use_tls`, `tls_certificate`, `tls_private_key` | `true`, `server/certs/server.{pem,key}` | PEM files from `deploy/gen-server-cert.sh` |
| `allowed_client_ids` | `[]` (any) | optional allow-list of client ids |
| `socket_timeout_ms` | `30000` | |

## How it works

The sections below follow the design document; each names the code that implements it.

### Client architecture - main loop, event queue, DB, config

`client/clientMain.c` is the main loop. On startup it loads the config, opens the state DB,
connects and authenticates, starts the watcher thread, and runs a full scan. It then loops:
drain watcher events into the pending batch, flush the batch when the batching rule fires, run
the periodic full scan, and reconnect with back-off whenever the transport drops - keeping the
unfinished part of the batch for retry. `--once` does one scan-and-flush pass and exits.

- **Event queue** (`client/eventQueue.c`): a mutex/condvar queue between the watcher thread and
  the main loop. Pushes de-duplicate on (kind, root, path), so a file rewritten a thousand times
  is one pending event.
- **State DB** (`client/db.c`): for every file the server has confirmed - its stat triple (size,
  mtime, inode), whole-file hash and chunk layout - plus the directories already announced.
  Persisted as a readable text file, rewritten atomically after each batch.
- **Config** (`client/config.c`): the JSON file above, parsed with the vendored `sj.h` reader;
  unknown keys are ignored so newer configs load on older clients.

### Session setup - authentication, versioning, watched directories

`client/session.c` and `server/serverSession.c`. On a new connection:

1. **HELLO / HELLO_ACK** - the client sends the protocol version range it supports, its
   capability bits (zstd) and its CDC parameters; the server picks the highest common version
   (or rejects), intersects capabilities, adopts the CDC parameters so it chunks pre-existing
   files identically, and issues a random nonce.
2. **AUTH / AUTH_ACK** - mutual challenge/response over the pre-shared key: the client sends
   `HMAC-SHA256(psk, serverNonce || clientId)` plus its own nonce; the server verifies it
   (constant-time), checks the client id against its allow-list, and answers with
   `HMAC-SHA256(psk, clientNonce || "server")` so a rogue server can't impersonate the real one.
3. **WATCH_ROOTS / _ACK** - the client announces one label per watched root (its basename); the
   server maps each to a sanitised directory under `output_directory`.

All of this runs inside TLS (`shared/transport.c`): the server presents a certificate, the
client verifies it against the pinned SHA-256 fingerprint. Trust is by pinning rather than a CA
because the deployment is a fixed 1:1 client/server pair with a self-signed certificate.

### Change detection - watcher interface, batching, full scans, quick guard

The watcher (`client/watcher.h`) is an interface with three backends selected at build/run time:

| backend | file | notes |
|---|---|---|
| inotify | `client/watcher_inotify.c` | Linux. inotify isn't recursive, so a watch is added per directory at start and for every directory created later - and the new subtree is walked, since files can land in it before the watch exists. `IN_Q_OVERFLOW` becomes `EVENT_OVERFLOW`. |
| ReadDirectoryChangesW | `client/watcher_win32.c` | Windows. One overlapped read per root (natively recursive), multiplexed on one thread; a zero-length completion (buffer overflow) becomes `EVENT_OVERFLOW`. |
| poll | `client/watcher_poll.c` | Any OS. Stat-walks the roots every `poll_interval_seconds` and diffs against a snapshot. Used on macOS dev hosts or when forced. |

Events flow into the queue and are **batched** (`batchShouldFlush` in `eventQueue.c`): a sync is
triggered only when `batch_max_events` distinct events are pending or `batch_window_seconds`
have passed since the last one. Batching also means that while the connection is down, changes
simply accumulate and are delivered on reconnect.

The **full recursive scan** (`client/scanner.c`) runs at startup, every `scan_interval_seconds`,
and whenever a backend reports overflow. It compares each file's stat triple against the DB and
queues anything new or different - covering lost events, directories created before their watch
existed, tainted reads, and everything that changed while the process wasn't running.

The **quick guard** against trivial changes is two-layered (`client/sync.c`): if a file's (size,
mtime, inode) match the DB it is skipped without being read; if they differ but the whole-file
hash matches the DB (a `touch`, or a rewrite with identical content), only the stat triple is
updated and nothing is sent.

### Bandwidth - transport, compression, content-defined chunking

**Transport** (`shared/transport.h`): three function pointers (`sendAll`, `recvAll`, `close`)
over which everything else is written. Backends: plain TCP, and TCP+TLS via OpenSSL. See
[Deviations](#deviations-from-the-design-and-known-limitations) for why QUIC isn't bundled.

**Compression** (`shared/compress.c`, `shared/protocol.c`): after a message is serialised and
before it hits the transport, payloads over 128 bytes are zstd-compressed if the session
negotiated it and the result is smaller; a header flag tells the receiver. Files are never
pre-compressed, since that would defeat chunk-level deduplication.

**Content-defined chunking** (`shared/cdc.c`): FastCDC-style. A gear-hash rolling hash is
computed byte by byte; a chunk boundary is cut when the low `cdc_mask_bits` bits of the hash are
zero, subject to min/max chunk sizes. Because boundaries depend on local content rather than
offsets, an insertion or deletion only changes the chunk(s) around the edit - everything after it
re-chunks identically (the unit test measures 199 of 200 chunks unchanged after a mid-file
insert). The chunker is incremental, so the client streams files through it from disk rather
than loading them into memory. Every chunk is identified by its SHA-256 (`shared/sha256.c`,
vendored and verified against standard test vectors); the whole file is hashed in the same pass.

The per-file exchange (`client/sync.c`, `server/serverSession.c`):

1. `FILE_MANIFEST` - client sends the path, size, whole-file hash and the ordered
   `{offset, length, hash}` chunk list.
2. `FILE_NEEDED` - the server consults its **content-addressed chunk store**
   (`server/chunkStore.c`, `output_directory/.chunks/`) and asks only for hashes it doesn't
   have. If a previous version of the file exists on disk it is chunked with the negotiated
   parameters first, so its unchanged regions count as "already had".
3. `CHUNK_DATA` x N / `CHUNK_ACK` x N - only the requested chunks are sent.
4. `FILE_FINISH` / `FILE_RESULT` - the server reassembles the file from the store, verifies the
   whole-file hash, and atomically renames it into place.

### Reliability - acknowledgements, resumption, reconnection, restarts

- **Per-chunk acknowledgements.** The server verifies each chunk's hash on receipt, stores it,
  and acks it; a hash mismatch or write failure is a nack, and the client re-sends nacked chunks
  for up to `chunk_retry_limit` rounds. Before sending, the client re-reads the chunk from disk
  and re-verifies its hash, so a file modified mid-transfer can't be sent in a mixed state.
- **Resumption.** Chunks are in the server's store the moment they're acked, so if the connection
  drops mid-file the next manifest for that file only asks for the chunks that hadn't arrived.
- **Client DB.** Files the server has already confirmed are never re-read, let alone re-sent,
  and the client never has to ask the server what it has.
- **Reconnection.** Any transport failure marks the session dead; the main loop reconnects with
  `reconnect_delay_seconds` back-off and the unfinished batch is retried from the failed event.
  The server goes back to `accept()` after every session, so a reconnecting client is served.
- **Process death / reboot.** `deploy/` contains a systemd unit, a cron line, a launchd plist
  and a Windows Scheduled Task script that restart the client (and server) automatically. The
  startup full scan then reconciles everything that changed while it was down. All on-disk writes
  (server files, chunk store, client DB) are atomic (temp file + rename), so a kill mid-write
  never leaves a half-written file.

### Integrity - two-tier verification, tainted reads, directory metadata

- **Tier one:** every chunk is content-addressed; the server refuses (nacks) any chunk whose
  bytes don't hash to the manifest entry. **Tier two:** the reassembled file is hashed and
  compared with the manifest's whole-file hash before it is renamed into place; a mismatch
  discards it and is reported to the client, which does not record the file as synced.
- **Tainted reads:** the client stats a file before and after chunking it; if size, mtime or
  inode moved, the read is discarded and the file is left for the next scan rather than sent in
  an inconsistent state. The same check happens per chunk at send time.
- **Directory metadata:** directories are recorded in the DB and sent as `DIR_META` records,
  sorted ahead of files in each batch. If a file does arrive before its parent directory, the
  server creates the parent from the file's path.
- **Path safety:** everything path-shaped that comes off the wire is validated (no absolute
  paths, drive letters, `..` or `.` components) before being joined to the output directory,
  and root labels are sanitised.

### Wire protocol reference

`shared/protocol.h`. Every message is a 16-byte header (`magic`, `type`, `flags`, raw length,
wire length) followed by an optionally-compressed payload; integers are little-endian.

| message | direction | payload |
|---|---|---|
| `HELLO` | C→S | version range, capabilities, CDC params, client id |
| `HELLO_ACK` | S→C | chosen version (0 = reject), capabilities, auth nonce |
| `AUTH` | C→S | HMAC over server nonce + client id, client nonce |
| `AUTH_ACK` | S→C | success, HMAC over client nonce |
| `WATCH_ROOTS` / `_ACK` | C→S / S→C | root labels / ok |
| `DIR_META` / `_ACK` | C→S / S→C | root index, mtime, relative path / ok |
| `FILE_MANIFEST` | C→S | root, path, size, mtime, whole-file hash, chunk descriptors |
| `FILE_NEEDED` | S→C | indices of chunks the server lacks |
| `CHUNK_DATA` / `CHUNK_ACK` | C→S / S→C | index + bytes / index + ok |
| `FILE_FINISH` / `FILE_RESULT` | C→S / S→C | - / success + reason |
| `BYE` | C→S | - |

## Testing

`ctest --test-dir build --output-on-failure` runs both suites.

**Unit tests** (`tests/unit_tests.c`, ~1,170 checks): SHA-256 and HMAC-SHA256 against RFC test
vectors; CDC tiling/bounds/determinism, streaming-equals-in-memory, the mid-file-insertion
property, empty and worst-case inputs; every payload builder/parser round trip plus truncation
and malformed-manifest rejection; protocol framing over a socketpair with compression and
bad-magic rejection; file utilities (atomic write, ranged reads, guarded reads, `mkdir -p`,
recursive walk) and relative-path safety; the string map; client and server config parsing
including validation errors and legacy single-string `directory_paths`; DB save/load round trip
and the stat-triple guard; event queue de-duplication, ordering, timeouts and the batching rule;
the scanner against a real temp tree; the chunk store's integrity check; root-label sanitising.

**Integration test** (`tests/integration.sh`, real binaries and sockets): generates a
certificate; initial sync of two roots with nested directories and an empty file, verified with
`diff -r`; no-op re-run; `touch`-only change sends nothing; mid-file insert and prepend re-send
only the changed chunks; new nested directory; client-side deletion is not mirrored; wrong PSK
and wrong certificate pin are refused; daemon mode with the native watcher (inotify on Linux,
poll on macOS) picks up creates, new directories and modifications and flushes them in batches;
the server is stopped and restarted under the running daemon and the change made during the
outage is delivered on reconnect; clean SIGTERM shutdown; final `diff -r`.

Results at the time of writing: all unit checks and all integration checks pass on macOS
(Apple Clang 21) and on Ubuntu 24.04 (GCC 13, in Docker). The Windows binaries cross-compile
warning-free with mingw-w64 13.

## Deviations from the design, and known limitations

- **QUIC.** The design specifies QUIC (MsQuic) for 0/1-RTT setup, stream multiplexing and
  built-in TLS. MsQuic is not available through Homebrew or Ubuntu's repositories, so it isn't
  bundled; the transport is TCP+TLS behind `shared/transport.h`, which a QUIC backend would
  implement with the same three calls. Confidentiality and integrity in transit are provided by
  TLS 1.2+; the RTT and head-of-line-blocking advantages are not.
- **Windows** is compile-checked (mingw-w64) but has not been run on a Windows host; the
  ReadDirectoryChangesW backend in particular is untested at runtime. Windows `stat` reports no
  inode, so the quick guard there is size + mtime only.
- **State DB** is a text file rewritten atomically after each batch - readable and adequate for
  tens of thousands of files, but a real deployment at scale would use SQLite.
- **Chunk store growth.** The server keeps every chunk it has ever verified (that is what makes
  resumption and cross-file deduplication free). There is no garbage collection yet.
- **One client at a time.** Per the 1:1 assumption the server serves one session at a time; a
  second connection waits in the listen backlog.
- **Not synced, by design:** deletions and renames-as-deletions (the old name stays on the
  server), file permissions and modification times, symlink targets.
- **Certificate pinning** is opt-in; with an empty pin the client warns and connects anyway.
  Set the pin from `deploy/gen-server-cert.sh` output before trusting a network.
- `hash_algorithm` accepts only `sha256`; the config key exists so a second algorithm can be
  added without a format change.

## Repository layout

```
shared/    platform shim, SHA-256, HMAC, CDC chunker, protocol framing + payloads,
           transport (TCP/TLS), zstd wrapper, file utilities, string map, JSON helpers
client/    main loop, config, state DB, event queue, watcher interface + 3 backends,
           scanner, session (auth/version/roots), sync engine, sample config + roots
server/    accept loop, config, per-connection session, content-addressed chunk store
tests/     unit tests, integration script
deploy/    certificate generator, systemd units, cron, launchd plist, Windows task
docs/      architecture diagram
```
