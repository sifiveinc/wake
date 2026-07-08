#!/bin/sh
# Test: --rm -r handles directories with untracked files correctly
# Wake should remove tracked files but leave the directory and untracked files

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build output_dir

fail() {
    echo "FAIL: $1" >&2
    rm -rf output_dir
    exit 1
}

# Create directory with Wake-tracked files
"${WAKE}" -q --no-tty makeFiles output_dir/tracked1.txt output_dir/tracked2.txt

# Verify tracked files exist
test -f output_dir/tracked1.txt || fail "output_dir/tracked1.txt not created"
test -f output_dir/tracked2.txt || fail "output_dir/tracked2.txt not created"

# Create untracked file (not created by Wake)
echo "untracked content" > output_dir/untracked.txt
test -f output_dir/untracked.txt || fail "output_dir/untracked.txt not created"

# Remove directory with -r
"${WAKE}" --rm -r output_dir 2>&1

# Tracked files should be removed
test -f output_dir/tracked1.txt && fail "output_dir/tracked1.txt still exists (should be removed)"
test -f output_dir/tracked2.txt && fail "output_dir/tracked2.txt still exists (should be removed)"

# Directory and untracked file should remain
test -d output_dir || fail "output_dir was removed (should remain due to untracked file)"
test -f output_dir/untracked.txt || fail "output_dir/untracked.txt was removed (should remain)"

echo "PASS: tracked files removed, directory with untracked file remains" >&2

# Clean up
rm -rf output_dir
