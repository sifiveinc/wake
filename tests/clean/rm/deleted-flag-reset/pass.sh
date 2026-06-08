#!/bin/sh
# Test: deleted flag is properly reset when file is re-created after being deleted

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"
WAKE="$(realpath "$WAKE")"

export WAKE_CAS=1

rm -rf wake.db* wake.log .wake .build file.txt

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# Create a file and add it to the database
"${WAKE}" -q --no-tty makeFile file.txt

# Verify it exists and deleted=0
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='file.txt'")
test "$DELETED" = "0" || fail "Initial file should have deleted=0, got: $DELETED"

# Remove the file (sets deleted=1 in database)
"${WAKE}" --rm file.txt

# Verify deleted=1
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='file.txt'")
test "$DELETED" = "1" || fail "After --rm, file should have deleted=1, got: $DELETED"

# Re-create the exact same file (same path, same hash)
"${WAKE}" -q --no-tty makeFile file.txt

# CRITICAL TEST: Verify deleted was reset to 0
DELETED=$(sqlite3 wake.db "SELECT deleted FROM files WHERE path='file.txt'")
test "$DELETED" = "0" || fail "After re-creating file, deleted should be reset to 0, got: $DELETED"

echo "PASS: deleted flag reset" >&2
