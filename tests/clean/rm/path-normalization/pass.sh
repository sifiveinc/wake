#!/bin/sh
# Test: --rm correctly normalizes paths (relative, absolute, from subdirectories)

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

# Get absolute path to wake binary before changing directories
WAKE="$(realpath "$WAKE")"

rm -rf wake.db* wake.log .wake .build subdir

fail() {
    echo "FAIL: $1" >&2
    rm -rf subdir
    exit 1
}

# Create a subdirectory with files
mkdir -p subdir
"${WAKE}" -q --no-tty makeFiles subdir/file1.txt subdir/file2.txt file3.txt

# Verify files were created
test -f subdir/file1.txt || fail "subdir/file1.txt not created"
test -f subdir/file2.txt || fail "subdir/file2.txt not created"
test -f file3.txt || fail "file3.txt not created"

# Test 1: Remove with relative path from workspace root
"${WAKE}" --rm subdir/file1.txt
test ! -f subdir/file1.txt || fail "subdir/file1.txt not removed (relative path)"

# Test 2: Remove with absolute path
"${WAKE}" --rm "$(pwd)/file3.txt"
test ! -f file3.txt || fail "file3.txt not removed (absolute path)"

# Test 3: Remove from subdirectory with relative path
cd subdir
"${WAKE}" --rm file2.txt
test ! -f file2.txt || fail "file2.txt not removed (from subdirectory)"
cd ..

echo "PASS: path normalization" >&2

# Clean up
rmdir subdir
