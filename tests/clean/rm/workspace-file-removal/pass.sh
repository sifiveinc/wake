#!/bin/sh
# Test: --rm removes workspace file

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

# Create output file
"${WAKE}" -q --no-tty makeFile output.txt

# Verify file exists
test -f output.txt || fail "output.txt not created"

# Run --rm
"${WAKE}" --rm output.txt

# File should be removed from workspace
test -f output.txt && fail "output.txt still exists after --rm"

"${WAKE}" --output output.txt --include-hidden
echo "PASS: workspace file removed" >&2

# Clean up
rm -f $RM_ARTIFACTS
