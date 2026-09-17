#!/bin/sh
# Test: --rm removes DB record for job output

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build output.txt

fail() {
    echo "FAIL: $1" >&2
    rm -f output.txt
    exit 1
}

# Create a job that outputs a file
"${WAKE}" -q --no-tty makeFile output.txt

# Verify file was created
test -f output.txt || fail "output.txt not created"

# Verify job is in database (query should find it)
"${WAKE}" --output output.txt --include-hidden || fail "job not in database before --rm"

# Remove the file from database
"${WAKE}" --rm output.txt

# Job should not be removed from database
"${WAKE}" --output output.txt --include-hidden || fail "job dropped from database after --rm"

echo "PASS: database job removed" >&2

# Clean up
rm -f output.txt
