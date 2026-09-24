#! /bin/sh

set -e
WAKE="${1:+$1/wake}"
ready_file="/tmp/wake-canceled-ready-$$"
trap 'rm -f "$ready_file"' EXIT

# Start from any empty db every time for stable job ids
rm -f wake.db
rm -f "$ready_file"

# Wait until the long-running job is actually running before cancelling Wake.
# A fixed timeout can fire during startup, before the recovery manifest exists.
export CANCELED_READY="$ready_file"
${WAKE} test &
wake_pid=$!
# Wait for the job to start before cancelling Wake.
for _ in $(seq 1 100); do
    if [ -f "$ready_file" ]; then
        break
    fi
    sleep 0.1
done
if [ ! -f "$ready_file" ]; then
    kill -TERM "$wake_pid" 2>/dev/null || true
    wait "$wake_pid" 2>/dev/null || true
    exit 1
fi
kill -TERM "$wake_pid" 2>/dev/null || true
wait "$wake_pid" 2>/dev/null || true

# Cancellation may take a moment to appear in inspection output.
for _ in $(seq 1 100); do
    ${WAKE} --canceled --simple 2>/dev/null && exit 0
    sleep 0.1
done

${WAKE} --canceled --simple
