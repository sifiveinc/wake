#!/bin/sh
# Test: CAS blob kept when multiple jobs produce same content

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf wake.db* wake.log .wake .build shared1.txt shared2.txt

function fail() {
    echo "FAIL: $1" >&2
    rm -f shared1.txt shared2.txt
    exit 1
}

# Create two files with identical content
"${WAKE}" -q --no-tty makeShared shared1.txt shared2.txt

# Both files should exist
test -f shared1.txt || fail "shared1.txt not created"
test -f shared2.txt || fail "shared2.txt not created"

# Count CAS blobs before removal (should be 1 blob shared by both)
BLOBS_BEFORE=$(find .build/cas -type f 2>/dev/null | wc -l)

# Remove one file
"${WAKE}" --rm shared1.txt

# shared1.txt should be gone, file2.txt should remain
test ! -f shared1.txt || fail "shared1.txt was *not* removed"
test -f shared2.txt || fail "shared2.txt was removed"

# CAS blob should still exist because shared2.txt references it
BLOBS_AFTER=$(find .build/cas -type f 2>/dev/null | wc -l)
test "$BLOBS_BEFORE" = "$BLOBS_AFTER" || fail "CAS blob was removed (had $BLOBS_BEFORE, now $BLOBS_AFTER)"

"${WAKE}" --output 'shared*.txt' --include-hidden
echo "PASS: shared CAS blob kept" >&2

# Clean up
rm -f shared1.txt shared2.txt
