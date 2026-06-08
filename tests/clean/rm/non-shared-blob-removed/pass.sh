#!/bin/sh
# Test: CAS blob removed when last (only) reference is removed

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf wake.db* wake.log .wake .build unique.txt

fail() {
    echo "FAIL: $1" >&2
    rm -f unique.txt
    exit 1
}

# Create a unique file
"${WAKE}" -q --no-tty makeUnique unique.txt

test -f unique.txt || fail "unique.txt not created"

# Count CAS blobs before removal
BLOBS_BEFORE=$(find .build/cas -type f 2>/dev/null | wc -l)
test "$BLOBS_BEFORE" -gt 0 || fail "no CAS blobs created"

# Remove the file
"${WAKE}" --rm unique.txt

# File should be gone
test ! -f unique.txt || fail "unique.txt not removed"

# CAS blob should be removed since no other job references it
BLOBS_AFTER=$(find .build/cas -type f 2>/dev/null | wc -l)
test "$BLOBS_AFTER" = "0" || fail "CAS blob not removed (had $BLOBS_BEFORE, now $BLOBS_AFTER)"

"${WAKE}" --output unique.txt --include-hidden
echo "PASS: CAS blob removed with last reference" >&2

# Clean up
rm -f unique.txt
