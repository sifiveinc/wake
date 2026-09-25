#! /bin/sh

set -e
WAKE="${1:+$1/wake}"
ready_file="/tmp/wake-canceled-ready-$$"
RM_ARTIFACTS=".wake/locks/* .build/cas/staging/recovery $ready_file"
cleanup() {
    if [ -n "${WAKE_PID:-}" ]; then
        kill "$WAKE_PID" 2>/dev/null || true
        wait "$WAKE_PID" 2>/dev/null || true
    fi
    rm -rf $RM_ARTIFACTS
}
trap cleanup EXIT

# Start from any empty db every time for stable job ids
rm -rf wake.db* wake.log .wake .build
cleanup

# Wait until the long-running job is actually running before cancelling Wake.
# A fixed timeout can fire during startup, before the recovery manifest exists.
export CANCELED_READY="$ready_file"
${WAKE} test &
WAKE_PID=$!
# Wait for the job to start before cancelling Wake.
for _ in $(seq 1 100); do
    if [ -f "$ready_file" ]; then
        break
    fi
    sleep 0.1
done
if [ ! -f "$ready_file" ]; then
    kill -TERM "$WAKE_PID" 2>/dev/null || true
    wait "$WAKE_PID" 2>/dev/null || true
    exit 1
fi
kill -TERM "$WAKE_PID" 2>/dev/null || true
wait "$WAKE_PID" 2>/dev/null || true
unset WAKE_PID

# Cancellation may take a moment to appear in inspection output.
for _ in $(seq 1 100); do
    ${WAKE} --canceled --simple 2>/dev/null && exit 0
    sleep 0.1
done

${WAKE} --canceled --simple
