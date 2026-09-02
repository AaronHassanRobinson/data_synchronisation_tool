Data synchronisation tool for offensive cyber security assessment, built around a design doc
exploring a custom rsync-alternative using content-defined chunking (CDC). See the
[Experimental/AI](#experimentalai) section below for where that CDC work stands.

![data_synchronisation_tool.drawio.png](docs/data_synchronisation_tool.drawio.png)

## Building

This is a CMake project (requires CMake >= 3.20 and a C23-capable compiler). Builds and runs on
both macOS (Apple Clang) and Linux (verified on Ubuntu 24.04 / GCC 13). Building from the repo
root generates both the `sync_client` and `sync_server` binaries:

```
cmake -B build -S .
cmake --build build
```

The resulting executables are placed at `build/client/sync_client` and `build/server/sync_server`.


## Experimental/AI
There exists an experimental branch to see how far AI could go with the design doc.

This branch picks up the codebase from its early proof-of-concept starting point and fleshes it
out into a working end-to-end demo of the design doc's content-defined chunking (CDC) transfer
mechanism. Everything else in the design (QUIC, compression, the on-disk state DB, the
file-system watcher, multi-directory support) is deliberately **not** implemented yet - see
[What's out of scope](#whats-out-of-scope-for-this-demo) below. The goal here was just to prove
the CDC diffing idea actually works over a real socket, not to build the whole system.

### Running the demo

In one terminal, start the server (args are optional - defaults shown):

```
./build/server/sync_server 9001 server_data
```

In another terminal, run the client from the repo root (so the config's relative paths resolve):

```
./build/client/sync_client client/clientConfig.json
```

The client reads `client/clientConfig.json`, connects to the server, and syncs every regular
file in `client/watched_dir/` (a couple of sample files are checked in). The server writes what
it reconstructs into `server_data/`. Both sides log a per-file chunk count and, on the server
side, how many of those chunks it already had.

To see CDC actually save bandwidth, edit `client/watched_dir/report.txt` - insert a line
somewhere in the middle, not just append to the end - and run the client again against the
same running server. The log will show something like:

```
-> report.txt (8947 bytes, 6 chunks, sha256 ca7a8d1c815c...)
   sent 3/6 chunks (50% reused from server's copy)
```

Only the chunks around the edit changed hash; everything before and after was re-chunked
identically and reused from the server's existing copy, even though every byte after the
insertion point shifted position in the file. A fixed-offset diff would have had to resend
almost the whole file for the same edit.

### How it works

**Chunking.** `shared/cdc.c` implements a small FastCDC-style chunker: a rolling "gear hash" is
computed byte-by-byte over the file, and a chunk boundary is cut whenever the low bits of that
hash are all zero (`CDC_MASK_BITS`), subject to `CDC_MIN_CHUNK_SIZE`/`CDC_MAX_CHUNK_SIZE`
bounds. Because the boundary is determined by local content rather than a fixed offset, an
insertion or deletion only perturbs the one or two chunks around the edit - everything else in
the file re-chunks identically, which is exactly what makes the delta sync above work. The gear
table (256 pseudo-random constants the rolling hash folds through) is generated at startup from
a fixed seed, so client and server always derive the same table without one being checked in as
a literal blob. Chunk sizes are deliberately tiny (64 B - 2 KB) so the small demo files actually
produce several chunks; a real deployment would size these in the KB-MB range.

**Protocol (plain TCP, see `shared/protocol.h`).** After a one-message version handshake, the
client drives one request/response cycle per file:

1. **`MSG_CDC_MANIFEST`** (client → server) - file name, size, whole-file hash, and the ordered
   list of `{offset, length, hash}` chunk descriptors.
2. **`MSG_CDC_NEEDED_CHUNKS`** (server → client) - the server re-chunks whatever it already has
   on disk at that path (this stands in for the design's persistent "last known server state"
   DB, recomputed on demand since persistence was out of scope here), compares chunk hashes
   against the manifest, and asks for only the indices it doesn't recognise.
3. **`MSG_CDC_CHUNK_DATA`** (client → server) - the raw bytes of just the requested chunks.
4. **`MSG_CDC_SYNC_COMPLETE`** (server → client) - the server rebuilds the file by combining
   chunks it already had with the ones it just received, hashes the result, and reports whether
   it matches the manifest's whole-file hash.

**Integrity.** This is the design doc's two-tier verification: each chunk is self-identified by
its content hash (used for the diff itself), and the whole reconstructed file is separately
hashed and checked against the manifest before being written. A mismatch discards the
reconstruction instead of writing a corrupt file. `shared/sha256.c` is a small vendored SHA-256
(verified against `shasum -a 256` on the empty string, `"abc"`, and a block-boundary test
vector) - added so chunk/file hashing didn't need to pull in a crypto library.

**Collection semantics.** The server never deletes or overwrites anything the client didn't just
send it a verified replacement for, and it never pushes state back to the client beyond "did
this file verify" - consistent with the design doc's "collection server" framing (accumulate,
don't mirror deletions).

**Reliability, in this scope.** `sendAll`/`recvAll` (`shared/protocol.c`) loop until a full
buffer is moved, since a single `send()`/`recv()` isn't guaranteed to move the whole buffer.
Writes on the server are atomic (`shared/fileUtil.c` writes to a `.tmp` file, then `rename()`s
it into place), so a crash mid-write can't leave a half-written file behind.

### What's out of scope for this demo

Kept out deliberately, per the brief:

- **Transport**: plain TCP, no QUIC, no TLS. The design's confidentiality/integrity-in-transit
  argument for QUIC still applies to a real deployment; this demo just doesn't need it to prove
  the chunking idea.
- **Compression**: chunks are sent raw.
- **Persistent DB**: the server recomputes "what do I already have" by re-chunking the file
  that's already on disk at sync time, rather than consulting a cached hash list. Fine at demo
  scale; a real deployment would want the cached version so the server isn't re-hashing
  potentially-large existing files on every sync.
- **Watching / batching**: the client does a one-shot directory scan, not a live
  inotify/ReadDirectoryChangesW watcher with event batching.
- **Multiple/recursive watched directories**: `directory_paths` in `clientConfig.json` is a
  single, non-recursive directory for this demo, not the design's "specific set of directories."
- **Multiple connections / retry-on-drop**: one connection, one pass over the directory, then
  exit. No reconnect-and-resume loop or cron/systemd wrapper yet.

### Notes

- `cmake_minimum_required` is set to 3.20 rather than a newer version, since nothing in the
  project actually needs anything more recent and it keeps the build portable across common
  Linux CMake versions as well as macOS.
- `PATH_MAX` comes from `<limits.h>` rather than a platform-specific header, so it resolves the
  same way on both macOS and Linux.
