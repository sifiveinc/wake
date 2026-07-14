#!/bin/sh
# Test that binding an AF_UNIX socket inside the CAS-backed FUSE workspace works.
# A process bind()s a unix socket (a mknod(S_IFSOCK) through FUSE), then a child
# connect()s to it and reads data back. Verifies:
# 1. stat() through FUSE reports the socket (S_IFSOCK)
# 2. bind/connect round-trips through the mount
# 3. no socket node is left behind in the workspace
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

# socket_roundtrip.py is served to the command as a visible CAS file. Stage it into the CAS
# blob store at the path derived from the fake hash in input.json before running.
HASH=0000000000000000000000000000000000000000000000000000000000000002
BLOB_DIR=.build/cas/blobs/$(echo "$HASH" | cut -c1-2)
mkdir -p "$BLOB_DIR"
cp socket_roundtrip.py "$BLOB_DIR/$(echo "$HASH" | cut -c3-)"

trap 'rm -f mysocket' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)

echo "$OUTPUT" | grep -q "SOCKET_ROUNDTRIP_OK" || { echo "FAIL: socket round-trip did not succeed"; echo "$OUTPUT"; exit 1; }
test ! -e mysocket || { echo "FAIL: socket node left behind in workspace"; exit 1; }

echo "PASS"
