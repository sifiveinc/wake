#!/bin/sh
# Test: --rm -r with child directory specified before parent
# Should remove both cleanly without warnings or errors

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf wake.db* wake.log .wake .build parent_dir

fail() {
    echo "FAIL: $1" >&2
    rm -rf parent_dir
    exit 1
}

# Create nested structure
"${WAKE}" -q --no-tty makeFiles \
    parent_dir/file1.txt \
    parent_dir/child/file2.txt \
    parent_dir/child/grandchild/file3.txt

# Verify structure exists
test -f parent_dir/file1.txt || fail "parent_dir/file1.txt not created"
test -f parent_dir/child/file2.txt || fail "parent_dir/child/file2.txt not created"
test -f parent_dir/child/grandchild/file3.txt || fail "parent_dir/child/grandchild/file3.txt not created"

# Remove child before parent (both should be removed cleanly)
"${WAKE}" --rm -r parent_dir/child parent_dir

# Everything should be removed
test -d parent_dir && fail "parent_dir still exists"
test -d parent_dir/child && fail "parent_dir/child still exists"
test -d parent_dir/child/grandchild && fail "parent_dir/child/grandchild still exists"

echo "PASS: child-before-parent removal succeeded" >&2

# Clean up
rm -rf parent_dir
