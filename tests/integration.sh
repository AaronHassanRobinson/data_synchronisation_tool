#!/usr/bin/env bash
# End-to-end test against the real binaries: TLS + pinning, PSK auth (good and bad), version
# negotiation, compression, nested directories, CDC delta re-sync, touch-only no-op, the
# polling watcher + batching in daemon mode, server restart / client reconnect, and that a
# deleted client file is never deleted on the server.
#
#   tests/integration.sh <sync_server> <sync_client>
set -u
SERVER_BIN="$1"
CLIENT_BIN="$2"
PORT=$(( 20000 + RANDOM % 20000 ))
# inotify on Linux, the portable poller elsewhere (the poller is what macOS dev hosts use)
if [ "$(uname -s)" = "Linux" ]; then WATCHER=native; WATCHER_NAME=inotify; else WATCHER=poll; WATCHER_NAME=poll; fi
WORK="$(pwd)/sync_integration_tmp_$$"
rm -rf "$WORK"; mkdir -p "$WORK"
FAILED=0
SERVER_PID=""; CLIENT_PID=""

pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; FAILED=$((FAILED + 1)); }
check() { if eval "$1"; then pass "$2"; else fail "$2"; fi; }
cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    wait 2>/dev/null
    if [ -n "${KEEP_WORK:-}" ]; then echo "work dir kept at $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

start_server() {
    "$SERVER_BIN" "$WORK/server.json" >> "$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do grep -q "listening on port" "$WORK/server.log" 2>/dev/null && break; sleep 0.1; done
}
stop_server() { kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; SERVER_PID=""; }
run_client_once() { "$CLIENT_BIN" "$WORK/client.json" --once > "$WORK/client-once.log" 2>&1; echo $?; }
# Compare a root and its server copy (the server also holds .chunks/, which is not part of the tree).
trees_match() { diff -r "$WORK/roots/$1" "$WORK/out/$1" > /dev/null 2>&1; }
# One-directional: every file on the client matches its server copy (the server may hold more,
# since deletions are never mirrored).
client_files_on_server() {
    local rc=0
    while IFS= read -r f; do
        cmp -s "$f" "$WORK/out/$1/${f#"$WORK/roots/$1/"}" || rc=1
    done < <(find "$WORK/roots/$1" -type f)
    return $rc
}
wait_for() { # wait_for <seconds> <command>
    local deadline=$(( $(date +%s) + $1 )); shift
    while ! eval "$@" 2>/dev/null; do [ "$(date +%s)" -ge "$deadline" ] && return 1; sleep 0.5; done
    return 0
}

echo "== setup =="
mkdir -p "$WORK/certs"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=test" -keyout "$WORK/certs/server.key" -out "$WORK/certs/server.pem" 2>/dev/null
PIN=$(openssl x509 -in "$WORK/certs/server.pem" -outform DER | openssl dgst -sha256 | awk '{print $NF}')
check '[ ${#PIN} -eq 64 ]' "generated server certificate (pin $PIN)"

mkdir -p "$WORK/roots/documents/nested/deeper" "$WORK/roots/logs"
head -c 60000 /dev/urandom > "$WORK/roots/documents/random.bin"
for i in $(seq 1 400); do echo "line $i: the quick brown fox jumps over the lazy dog"; done > "$WORK/roots/documents/report.txt"
echo "small" > "$WORK/roots/documents/nested/deeper/tiny.txt"
echo "hello" > "$WORK/roots/documents/nested/hello.txt"
: > "$WORK/roots/documents/empty.txt"
printf 'log line 1\nlog line 2\n' > "$WORK/roots/logs/app.log"

cat > "$WORK/server.json" <<JSON
{ "listen_port": $PORT, "output_directory": "$WORK/out", "pre_shared_key": "integration-secret",
  "use_tls": true, "tls_certificate": "$WORK/certs/server.pem", "tls_private_key": "$WORK/certs/server.key",
  "allowed_client_ids": ["it-client"], "socket_timeout_ms": 10000 }
JSON
write_client_config() { # write_client_config <psk> <pin>
cat > "$WORK/client.json" <<JSON
{ "server_ip_address": "127.0.0.1", "server_port": $PORT, "client_id": "it-client", "pre_shared_key": "$1",
  "use_tls": true, "server_certificate_sha256": "$2", "use_compression": true,
  "directory_paths": ["$WORK/roots/documents", "$WORK/roots/logs"], "database_path": "$WORK/client.db",
  "watcher_backend": "$WATCHER", "poll_interval_seconds": 1, "scan_interval_seconds": 3600,
  "batch_max_events": 100, "batch_window_seconds": 1, "reconnect_delay_seconds": 1, "socket_timeout_ms": 10000,
  "cdc_min_chunk_size": 256, "cdc_max_chunk_size": 4096, "cdc_mask_bits": 10 }
JSON
}
write_client_config "integration-secret" "$PIN"
start_server

echo "== initial full sync (--once) =="
check '[ "$(run_client_once)" = "0" ]' "client --once exits 0"
check 'grep -q "protocol v1, TLS, compression on" "$WORK/client-once.log"' "negotiated TLS + compression + v1"
check 'trees_match documents' "documents root replicated byte-for-byte (incl. nested dirs + empty file)"
check 'trees_match logs' "logs root replicated"
check 'grep -q "file random.bin" "$WORK/server.log"' "server logged the transfer"
check '[ -d "$WORK/out/.chunks" ]' "server chunk store created"
check 'grep -q "^F " "$WORK/client.db"' "client state DB written"

echo "== no-op re-sync =="
check '[ "$(run_client_once)" = "0" ]' "second --once exits 0"
check 'grep -q "files (0 queued)" "$WORK/client-once.log" && grep -q "nothing to sync" "$WORK/client-once.log"' "nothing re-sent when nothing changed (quick guard)"

echo "== touch-only change (mtime moves, content identical) =="
sleep 1.1; touch "$WORK/roots/documents/random.bin"
check '[ "$(run_client_once)" = "0" ]' "--once after touch exits 0"
check 'grep -q "0 synced, 1 unchanged" "$WORK/client-once.log"' "touch detected by stat, defeated by hash: no transfer"

echo "== CDC delta re-sync after a mid-file insert =="
python3 - "$WORK/roots/documents/report.txt" <<'PY'
import sys; p = sys.argv[1]; s = open(p).read(); i = s.index("line 200:")
open(p, "w").write(s[:i] + ">>> inserted block shifting every later byte <<<\n" + s[i:])
PY
head -c 100 /dev/urandom | cat - "$WORK/roots/documents/random.bin" > "$WORK/roots/documents/random.new" && mv "$WORK/roots/documents/random.new" "$WORK/roots/documents/random.bin"
check '[ "$(run_client_once)" = "0" ]' "--once after edits exits 0"
check 'trees_match documents' "edited files replicated correctly"
REPORT_LINE=$(grep "file report.txt" "$WORK/client-once.log")
echo "     $REPORT_LINE"
check 'echo "$REPORT_LINE" | grep -Eq "server already had [1-9]"' "report.txt: most chunks reused despite shifted offsets"
RANDOM_LINE=$(grep "file random.bin" "$WORK/client-once.log")
echo "     $RANDOM_LINE"
check 'echo "$RANDOM_LINE" | grep -Eq "server already had [1-9]"' "random.bin: prepend only re-sent the leading chunks"

echo "== new nested directory + file =="
mkdir -p "$WORK/roots/logs/2026/09"
echo "new log" > "$WORK/roots/logs/2026/09/day.log"
check '[ "$(run_client_once)" = "0" ]' "--once with new subtree exits 0"
check 'trees_match logs' "new nested directory and file replicated"
check 'grep -q "dir  2026/09" "$WORK/client-once.log"' "directory metadata records sent"

echo "== deletions are NOT mirrored (collection server) =="
rm "$WORK/roots/documents/nested/hello.txt"
check '[ "$(run_client_once)" = "0" ]' "--once after delete exits 0"
check '[ -f "$WORK/out/documents/nested/hello.txt" ]' "server kept the deleted file"

echo "== authentication and pinning =="
write_client_config "wrong-secret" "$PIN"
check '[ "$(run_client_once)" != "0" ]' "wrong pre-shared key: client fails"
check 'grep -q "authentication FAILED" "$WORK/server.log"' "server logged the auth failure"
write_client_config "integration-secret" "0000000000000000000000000000000000000000000000000000000000000000"
check '[ "$(run_client_once)" != "0" ]' "wrong certificate pin: client refuses to talk"
check 'grep -q "does not match pinned" "$WORK/client-once.log"' "client logged the pin mismatch"
write_client_config "integration-secret" "$PIN"

echo "== daemon mode: watcher + batching =="
"$CLIENT_BIN" "$WORK/client.json" > "$WORK/client-daemon.log" 2>&1 &
CLIENT_PID=$!
check 'wait_for 15 grep -q "watcher: $WATCHER_NAME backend running" "$WORK/client-daemon.log"' "daemon started with the $WATCHER_NAME watcher"
sleep 1
echo "created while watching" > "$WORK/roots/documents/live.txt"
mkdir -p "$WORK/roots/documents/livedir"; echo "in new dir" > "$WORK/roots/documents/livedir/x.txt"
check 'wait_for 20 cmp -s "$WORK/roots/documents/live.txt" "$WORK/out/documents/live.txt"' "watcher picked up a new file and synced it"
check 'wait_for 20 cmp -s "$WORK/roots/documents/livedir/x.txt" "$WORK/out/documents/livedir/x.txt"' "watcher picked up a new directory + file"
echo "modified while watching" >> "$WORK/roots/documents/live.txt"
check 'wait_for 20 cmp -s "$WORK/roots/documents/live.txt" "$WORK/out/documents/live.txt"' "watcher picked up a modification"
check 'grep -q "batch: flushing" "$WORK/client-daemon.log"' "events were flushed in batches"

echo "== server restart while the daemon is running (reconnect) =="
stop_server
sleep 1
echo "written while server down" > "$WORK/roots/logs/offline.log"
sleep 3
start_server
check 'wait_for 30 cmp -s "$WORK/roots/logs/offline.log" "$WORK/out/logs/offline.log"' "client reconnected and delivered the queued change"
check 'grep -q "connection failed\|connection lost" "$WORK/client-daemon.log"' "client logged the outage"

echo "== clean shutdown =="
kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID"; CLIENT_EXIT=$?; CLIENT_PID=""
check '[ "$CLIENT_EXIT" = "0" ]' "daemon exits 0 on SIGTERM"
check 'grep -q "client: shutting down" "$WORK/client-daemon.log"' "daemon shut down cleanly"
check 'client_files_on_server documents && client_files_on_server logs' "final state: every client file matches its server copy"

echo
if [ "$FAILED" -eq 0 ]; then echo "integration: all checks passed"; else echo "integration: $FAILED check(s) FAILED"; fi
[ "$FAILED" -eq 0 ]
