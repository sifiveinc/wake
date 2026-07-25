#!/bin/sh
# Test: unsafe_removeFiles skips non-recursive directories

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build dir

fail() {
    echo "FAIL: $1" >&2
    rm -rf dir
    exit 1
}

# Create a nested directory tree.
"${WAKE}" -q --no-tty initTest || fail "initTest failed"

test -f "dir/file1.txt" || fail "dir/file1.txt not created"
test -f "dir/child/file2.txt" || fail "dir/child/file2.txt not created"

# Non-recursive removal request should be a no-op and still succeed.
"${WAKE}" -q --no-tty runTest 2>&1 || fail "runTest failed"

# Directory and children should still exist after the call.
test -d dir || fail "dir was removed unexpectedly"
test -f "dir/file1.txt" || fail "dir/file1.txt was removed unexpectedly"
test -f "dir/child/file2.txt" || fail "dir/child/file2.txt was removed unexpectedly"

echo "PASS: rm_generated non-recursive directory skip" >&2

# Clean up
rm -rf dir
