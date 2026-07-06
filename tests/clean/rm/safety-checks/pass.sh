#!/bin/sh
# Test: Safety checks reject dangerous paths

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

# Test 1: Reject workspace root (.)
"${WAKE}" --rm . 2>&1 && fail "should reject '.'"

# Test 2: Reject workspace root from subdirectory (..)
mkdir -p subdir
cd subdir
"${WAKE}" --rm .. 2>&1 && fail "should reject '..' pointing to the workspace root"
cd ..

# Test 3: Reject wake.db
"${WAKE}" --rm wake.db 2>&1 && fail "should reject 'wake.db'"

# Test 4: Reject .build/cas directory
"${WAKE}" --rm .build/cas 2>&1 && fail "should reject '.build/cas'"

# Test 5: Reject files under .build/cas
"${WAKE}" --rm .build/cas/somefile 2>&1 && fail "should reject '.build/cas/somefile'"

# Test 6: Allow .build/other (not under cas/)
"${WAKE}" -q --no-tty -x 'write ".build/other" ""'
"${WAKE}" --rm .build/other 2>&1 || fail "should *not* reject '.build/other'"
test -f .build/other && fail ".build/other still exists"

echo "PASS: safety checks" >&2

# Clean up
rmdir subdir
