#!/bin/sh
# Test: --rm handles duplicate paths on command line correctly
# Should remove files and directories cleanly without errors or duplicate warnings

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build dir1 file.txt

fail() {
    echo "FAIL: $1" >&2
    rm -rf dir1 file.txt
    exit 1
}

# Create directory with files, and a standalone file
"${WAKE}" -q --no-tty makeFiles \
    dir1/file1.txt \
    dir1/file2.txt \
    file.txt

# Verify files exist
test -f dir1/file1.txt || fail "dir1/file1.txt not created"
test -f dir1/file2.txt || fail "dir1/file2.txt not created"
test -f file.txt || fail "file.txt not created"

# Remove with duplicate paths on command line
# Test duplicates for both files and directories
"${WAKE}" --rm -r dir1 dir1 file.txt file.txt

# All files and directories should be removed
test -d dir1 && fail "dir1 still exists"
test -f file.txt && fail "file.txt still exists"

echo "PASS: duplicate paths (files and directories) handled correctly" >&2

# Clean up
rm -rf dir1 file.txt
