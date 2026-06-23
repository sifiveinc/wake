#!/bin/sh
# Test: Cache reuse works when one file sharing a blob is deleted but the blob remains available

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf wake.db* wake.log .wake .build shared1.txt shared2.txt

fail() {
    echo "FAIL: $1" >&2
    rm -f shared1.txt shared2.txt
    exit 1
}

# Create two files with identical content (they will share a CAS blob).
"${WAKE}" -q --no-tty makeShared shared1.txt shared2.txt
"${WAKE}" --output shared1.txt | sed 's:\$ \(\(/usr\)\?/bin/\)dash:$ dash:'

# Remove shared1.txt (blob should remain because shared2.txt still references it).
"${WAKE}" --rm shared1.txt

# Now try to run the job that produces shared1.txt again; it should reuse the cached Job because
# the blob is still available in the CAS.
"${WAKE}" -q --no-tty makeShared shared1.txt

# The file should be rematerialized, and the job information not indicate a new execution.
test -f shared1.txt || fail "shared1.txt was not rematerialized"
"${WAKE}" --output shared1.txt | sed 's:\$ \(\(/usr\)\?/bin/\)dash:$ dash:'

# Verify that the deleted flag was reset to 0 when the file was rematerialized.
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='shared1.txt'")
test "$DELETED" = "0" || fail "After rematerialization, deleted should be 0, got: $DELETED"

echo "PASS: cache reused with shared blob" >&2

# Clean up
rm -f shared1.txt shared2.txt
