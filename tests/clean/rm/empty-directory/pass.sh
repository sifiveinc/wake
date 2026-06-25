#!/bin/sh
# Test: --rm -r removes empty directories cleanly

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build empty_dir

fail() {
    echo "FAIL: $1" >&2
    rm -rf empty_dir
    exit 1
}

# Create an empty directory via Wake (directory will be in database as a job output)
"${WAKE}" -q --no-tty -x 'mkdir "empty_dir"'

# Verify directory exists and is empty
test -d empty_dir || fail "empty_dir not created"
test -z "$(ls -A empty_dir)" || fail "empty_dir is not empty"

# Remove empty directory with -r
"${WAKE}" --rm -r empty_dir

# Directory should be removed
test -d empty_dir && fail "empty_dir still exists after --rm -r"

echo "PASS: empty directory removed cleanly" >&2

# Clean up
rm -rf empty_dir
