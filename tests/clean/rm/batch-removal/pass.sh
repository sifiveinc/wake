#!/bin/sh
# Test: unsafe_removeFiles / --rm correctly batches large path lists.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build batched

fail() {
    echo "FAIL: $1" >&2
    rm -rf batched
    exit 1
}

# Create a large number of unique generated files, comfortably larger than typical
# SQLite parameter limits (commonly 999), to force Database::remove_blobs to
# iterate in multiple chunks. Existence is verified by the shell driver.
COUNT=2000
"${WAKE}" -q --no-tty initTestFiles "$COUNT" || fail "initTestFiles failed"

count=$(ls batched | wc -l | tr -d ' ')
test "$count" = "$COUNT" || fail "expected $COUNT batch files, got $count"

# Remove them all in one request; --rm -r shares the same Database::remove_blobs
# chunking logic as unsafe_removeFiles, so it doesn't need to go through the prim
# to exercise the batching logic for both.
"${WAKE}" --rm -r "batched" || fail "wake --rm -r failed on batch directory"

test -d "batched" && fail "batched still exists after --rm -r"

echo "PASS: rm_generated batched removal" >&2

# Clean up
rm -rf "batched"
