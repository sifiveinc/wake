#!/bin/sh
# Test --history output format for completed, running, and crashed runs

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

echo "Skipping test until https://github.com/sifiveinc/wake/issues/1781 is resolved"
exit 0

cleanup() {
  if [ -n "${WAKE_PID:-}" ]; then
    kill $WAKE_PID 2>/dev/null || true
    wait $WAKE_PID 2>/dev/null || true
  fi
  rm -rf wake.db* wake.log .wake .started
}
trap cleanup EXIT
cleanup

# Run 1: A quick completed build
"${WAKE}" -x 'True'

# Verify completed run shows duration in brackets
OUTPUT=$("${WAKE}" --history)
if echo "$OUTPUT" | grep -qE '\[[0-9]+s\].*True'; then
  echo "PASS: completed run shows duration"
else
  echo "FAIL: expected completed run with duration"
  echo "$OUTPUT"
  exit 1
fi

# Run 2: Start a long-running build
"${WAKE}" -x 'longRunningBuild Unit' &
WAKE_PID=$!

# Wait for it to start
while [ ! -f .started ]; do sleep 1; done

# Verify running build shows [running]
OUTPUT=$("${WAKE}" --history)
if echo "$OUTPUT" | grep -qE '\.\.\.[[:space:]]+\[running\].*longRunningBuild'; then
  echo "PASS: running build shows [running]"
else
  echo "FAIL: expected running build marker"
  echo "$OUTPUT"
  kill $WAKE_PID 2>/dev/null || true
  exit 1
fi

# Kill the build (simulate crash)
kill -9 $WAKE_PID 2>/dev/null || true
wait $WAKE_PID 2>/dev/null || true
unset WAKE_PID

# Before reaping, still shows as running (lock file gone but not reaped yet)
# After any wake command, it gets reaped and shows as crashed
"${WAKE}" -x 'True'

OUTPUT=$("${WAKE}" --history)
if echo "$OUTPUT" | grep -qE '\?\?\?[[:space:]]+\[crashed\].*longRunningBuild'; then
  echo "PASS: crashed build shows [crashed]"
else
  echo "FAIL: expected crashed build marker"
  echo "$OUTPUT"

  exit 1
fi

echo "All history format tests passed"

