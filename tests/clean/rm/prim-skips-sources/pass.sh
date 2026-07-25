#!/bin/sh
# Test: unsafe_removeFiles skips files published to wake's source topic

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build generated.txt

fail() {
    echo "FAIL: $1" >&2
    rm -rf generated.txt
    exit 1
}

# Create the fixture source file and both generated files.
"${WAKE}" -q --no-tty initTest || fail "initTest failed"

test -f generated.txt || fail "generated.txt not created"

# Request removal of the source together with a generated file.
# The source should be skipped (excluded) while the generated file is removed.
"${WAKE}" -q --no-tty runTest stdout generated.txt || fail "runTest failed"

test -f stdout || fail "stdout was removed but should have been excluded as a source"
test -f generated.txt && fail "generated.txt still exists after removal"

echo "PASS: unsafe_removeFiles skips sources" >&2

# Clean up
rm -rf generated.txt
