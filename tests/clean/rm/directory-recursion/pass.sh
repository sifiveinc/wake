#!/bin/sh
# Test: --rm without -r skips directories

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build output_dir

fail() {
    echo "FAIL: $1" >&2
    rm -rf output_dir
    exit 1
}

# Create directory with files
"${WAKE}" -q --no-tty makeFiles \
    parent_dir/child1/file1.txt \
    parent_dir/child1/grandchild/file2.txt \
    parent_dir/child2/file3.txt
# parent_dir/
#   child1/
#     file1.txt
#     grandchild/
#       file2.txt
#   child2/
#     file3.txt

# Verify nested structure exists
test -d parent_dir || fail "parent_dir not created"
test -d parent_dir/child1 || fail "parent_dir/child1 not created"
test -d parent_dir/child1/grandchild || fail "parent_dir/child1/grandchild not created"
test -d parent_dir/child2 || fail "parent_dir/child2 not created"
test -f parent_dir/child1/file1.txt || fail "parent_dir/child1/file1.txt not created"
test -f parent_dir/child1/grandchild/file2.txt || fail "parent_dir/child1/grandchild/file2.txt not created"
test -f parent_dir/child2/file3.txt || fail "parent_dir/child2/file3.txt not created"

# Try to remove directory without -r (should skip it and print the message for the golden stderr).
"${WAKE}" --rm parent_dir 2>&1

# Directories and files should still exist
test -d parent_dir || fail "parent_dir should have been skipped"
test -d parent_dir/child1 || fail "parent_dir/child1 should have been skipped"
test -d parent_dir/child1/grandchild || fail "parent_dir/child1/grandchild should have been skipped"
test -d parent_dir/child2 || fail "parent_dir/child2 should have been skipped"
test -f parent_dir/child1/file1.txt || fail "parent_dir/child1/file1.txt should have been skipped"
test -f parent_dir/child1/grandchild/file2.txt || fail "parent_dir/child1/grandchild/file2.txt should have been skipped"
test -f parent_dir/child2/file3.txt || fail "parent_dir/child2/file3.txt should have been skipped"

# Remove parent directory recursively
"${WAKE}" --rm -r parent_dir

# Everything should be removed
test -d parent_dir && fail "parent_dir still exists after --rm -r"
test -d parent_dir/child1 && fail "parent_dir/child1 still exists"
test -d parent_dir/child1/grandchild && fail "parent_dir/child1/grandchild still exists"
test -d parent_dir/child2 && fail "parent_dir/child2 still exists"
test -f parent_dir/child1 && fail "parent_dir/child1 still exists"
test -f parent_dir/child1/grandchild && fail "parent_dir/child1/grandchild still exists"
test -f parent_dir/child2 && fail "parent_dir/child2 still exists"

echo "PASS: directory removed with and only with -r" >&2

# Clean up
rm -rf output_dir
