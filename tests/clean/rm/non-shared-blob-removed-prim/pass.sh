#!/bin/sh
# Test: rm_generated / unsafe_removeFiles basic removal

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build dir file.txt

fail() {
    echo "FAIL: $1" >&2
    rm -rf dir file.txt
    exit 1
}

# Create the file and directory tree to be removed.
"${WAKE}" -q --no-tty initTest || fail "initTest failed"

test -f file.txt || fail "file.txt not created"
test -f "dir/child1.txt" || fail "dir/child1.txt not created"
test -f "dir/sub/child2.txt" || fail "dir/sub/child2.txt not created"

# Remove the file (non-recursively) and the directory tree (recursively).
"${WAKE}" -q --no-tty runTest || fail "runTest failed"

test -f file.txt && fail "file.txt still exists after unsafe_removeFiles"
test -d dir && fail "dir still exists after recursive removal"

# Idempotency: calling again on already-removed paths should succeed and do nothing.
"${WAKE}" -q --no-tty runTest || fail "second removal failed unexpectedly"

test -f file.txt && fail "file.txt reappeared after idempotent removal"
test -d dir && fail "dir reappeared after idempotent removal"

echo "PASS: rm_generated basic removal" >&2

# Clean up
rm -rf dir file.txt
