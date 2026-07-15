#!/bin/sh
# Test that creating a FIFO (named pipe) via mknod/mkfifo inside the CAS-backed FUSE workspace
# works. FIFOs are ephemeral special nodes handled the same way as sockets and device nodes:
# a real backing node is created and the virtualized getattr reports S_IFIFO. Verifies:
# 1. stat() through FUSE reports the fifo (S_IFIFO)
# 2. a writer and reader can round-trip data through the fifo
# 3. no fifo node is left behind in the workspace
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

# fifo_roundtrip.py is served to the command as a visible CAS file. Stage it into the CAS
# blob store at the path derived from the fake hash in input.json before running.
HASH=0000000000000000000000000000000000000000000000000000000000000001
BLOB_DIR=.build/cas/blobs/$(echo "$HASH" | cut -c1-2)
mkdir -p "$BLOB_DIR"
cp fifo_roundtrip.py "$BLOB_DIR/$(echo "$HASH" | cut -c3-)"

trap 'rm -f myfifo' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)

echo "$OUTPUT" | grep -q "FIFO_ROUNDTRIP_OK" || { echo "FAIL: fifo round-trip did not succeed"; echo "$OUTPUT"; exit 1; }
test ! -e myfifo || { echo "FAIL: fifo node left behind in workspace"; exit 1; }

echo "PASS"
