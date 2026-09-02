#!/usr/bin/env bash
# Generates a self-signed TLS certificate for the server and prints the SHA-256 fingerprint the
# client should pin (server_certificate_sha256 in clientConfig.json).
#
#   deploy/gen-server-cert.sh [output-dir] [common-name]
set -euo pipefail
OUT="${1:-server/certs}"
CN="${2:-sync-server}"
mkdir -p "$OUT"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=$CN" \
    -keyout "$OUT/server.key" -out "$OUT/server.pem" 2>/dev/null
chmod 600 "$OUT/server.key"
FINGERPRINT=$(openssl x509 -in "$OUT/server.pem" -outform DER | openssl dgst -sha256 | awk '{print $NF}')
echo "certificate: $OUT/server.pem"
echo "private key: $OUT/server.key"
echo "pin this in clientConfig.json -> \"server_certificate_sha256\": \"$FINGERPRINT\""
