#!/bin/sh
# Test: CAS blob kept when multiple jobs produce same content

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build shared1.txt shared2.txt

fail() {
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
test -f shared2.txt || fail "shared2.txt was removed by first --rm"

# CAS blob should still exist because shared2.txt references it
BLOBS_AFTER=$(find .build/cas -type f 2>/dev/null | wc -l)
test "$BLOBS_BEFORE" = "$BLOBS_AFTER" || fail "CAS blob was removed after first use (had $BLOBS_BEFORE, now $BLOBS_AFTER)"

# Now remove the other use of that blob -- it *should* now be removed from the CAS as that had
# been its final use.
"${WAKE}" --rm shared2.txt
test ! -f shared2.txt || fail "shared2.txt was *not* removed by second --rm"
BLOBS_AFTER=$(find .build/cas -type f 2>/dev/null | wc -l)
test "$BLOBS_BEFORE" -gt "$BLOBS_AFTER" || fail "CAS blob was *not* removed after last use (had $BLOBS_BEFORE, now $BLOBS_AFTER)"

"${WAKE}" --output 'shared*.txt' --include-hidden
echo "PASS: shared CAS blob kept" >&2

# Clean up
rm -f shared1.txt shared2.txt
