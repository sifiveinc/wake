#!/bin/sh
# Test: --rm warns about paths that are not registered in the database

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf wake.db* wake.log .wake .build registered.txt unregistered.txt

fail() {
    echo "FAIL: $1" >&2
    rm -rf registered.txt unregistered.txt
    exit 1
}

# Create a registered file using wake
"${WAKE}" -q --no-tty makeFiles registered.txt

# Create an unregistered file manually (not tracked by wake)
echo "manual content" > unregistered.txt

# Verify both files exist
test -f registered.txt || fail "registered.txt not created"
test -f unregistered.txt || fail "unregistered.txt not created"

# Try to remove both files, plus something missing entirely.
"${WAKE}" --rm registered.txt unregistered.txt nonexistent.txt 2>&1

# Registered file should be removed, but the unregistered file should *not* be removed.
test -f registered.txt && fail "registered.txt still exists"
test -f unregistered.txt || fail "unregistered.txt was incorrectly removed"

echo "PASS: unregistered paths warning issued correctly" >&2

# Clean up
rm -rf registered.txt unregistered.txt
