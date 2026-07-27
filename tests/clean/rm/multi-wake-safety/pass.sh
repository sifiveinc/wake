#!/bin/sh
# Test: unsafe_removeFiles skips files that are still claimed via run_files by
# another active Wake run, leaving both the workspace file and its CAS blob
# intact (multi-wake safety).

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build shared.txt shared.caspath

fail() {
    echo "FAIL: $1" >&2
    if [ -n "${RUN_A_PID:-}" ]; then
        kill "$RUN_A_PID" 2>/dev/null || true
    fi
    rm -rf shared.txt shared.caspath
    exit 1
}

# Start Run A in the background: it creates the shared CAS-backed file and
# then sleeps, keeping its run_id active while another Wake process runs. Its
# output is discarded since we kill it once done with it, and its own exit
# status/messages (e.g. from the termination signal) are irrelevant to the test.
"${WAKE}" -q --no-tty createShared >/dev/null 2>&1 &
RUN_A_PID=$!

# Wait until the CAS path file appears (written after the shared file itself) so we
# know the shared output has been created and registered in the database.
tries=0
while [ ! -f "shared.caspath" ]; do
  tries=$((tries + 1))
  if [ "$tries" -gt 20 ]; then
    fail "shared.caspath did not appear in time"
  fi
  sleep 1
done

CAS_PATH=$(cat "shared.caspath")

test -f "shared.txt" || fail "shared.txt missing before removal run"
test -f "$CAS_PATH" || fail "CAS blob missing before removal run"

# Run B: attempt rm_generated-style cleanup while Run A is still sleeping.
"${WAKE}" --no-tty tryRmWhileActive || fail "tryRmWhileActive failed"

# The workspace file must remain: it is claimed via run_files by the still-active
# Run A, so unsafe_removeFiles should skip it entirely rather than unlink it.
test -f "shared.txt" || fail "shared.txt was removed despite another active run"

# The CAS blob must also remain, since the file was never marked deleted.
test -f "$CAS_PATH" || fail "CAS blob was removed despite another active run"

# Run A is just sleeping to hold its run_id active; we don't need it to finish
# cleanly, so kill it rather than waiting it out.
kill "$RUN_A_PID" 2>/dev/null || true

echo "PASS: unsafe_removeFiles skipped the file while another run was active" >&2

# Clean up
rm -rf shared.txt shared.caspath
