#!/bin/sh
# Test: --rm works when workspace file doesn't exist

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS="output.txt"
rm -rf wake.db* wake.log .wake .build $RM_ARTIFACTS

fail() {
    echo "FAIL: $1" >&2
    rm -f $RM_ARTIFACTS
    exit 1
}

# Create file
"${WAKE}" -q --no-tty makeFile output.txt

test -f output.txt || fail "output.txt not created"

# Verify job is in database
"${WAKE}" --output output.txt --include-hidden || fail "job not in database before removal"

# Manually delete the workspace file
rm -f output.txt
test ! -f output.txt || fail "output.txt still exists"

# Run --rm even though file is already gone
"${WAKE}" --rm output.txt

# Job should not be removed from database
"${WAKE}" --output output.txt --include-hidden || fail "job dropped from database after --rm"

echo "PASS: --rm works with missing file" >&2

# Clean up
rm -f $RM_ARTIFACTS
