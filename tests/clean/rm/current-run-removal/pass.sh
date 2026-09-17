#!/bin/sh
# Test: rm_generated current-run CAS blob removal

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

# Ensure a clean workspace and CAS.
rm -rf wake.db* wake.log .wake .build file.txt file.caspath

fail() {
    echo "FAIL: $1" >&2
    rm -rf file.txt file.caspath
    exit 1
}

export WAKE_CAS=1

# Run a single Wake invocation that both creates the file and removes it via
# unsafe_removeFiles while the run is still active.
"${WAKE}" --no-tty runTest || fail "runTest failed"

test -f file.caspath || fail "file.caspath not created"
[ -n file.caspath ] || fail "CAS path was empty"

# Both the workspace file and its CAS blob should be deleted.
test ! -f file.txt || fail "file.txt still exists after unsafe_removeFiles"
test ! -f "$(cat file.caspath)" || fail "CAS blob still exists after file removal"

echo "PASS: rm_generated removed blob used only by current run" >&2

# Clean up
rm -rf file.txt file.caspath
