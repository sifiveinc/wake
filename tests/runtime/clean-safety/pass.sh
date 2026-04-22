#!/bin/sh
# --clean should refuse while a build is active, succeed after reaping dead runs

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

# longRunningBuild: first job creates .started, second sleeps forever
"${WAKE}" -x 'longRunningBuild Unit' &
WAKE_PID=$!

# Wait for first job to complete (wake has recorded it)
while [ ! -f .started ]; do sleep 1; done

# --clean should refuse (second job still running)
if "${WAKE}" --clean 2>&1 | grep -q "in progress"; then
  echo "PASS: --clean refused while build active"
else
  echo "FAIL: --clean should have refused"
  exit 1
fi

# Kill wake (simulates crash)
kill -9 $WAKE_PID 2>/dev/null || true
wait $WAKE_PID 2>/dev/null || true
unset WAKE_PID

# --clean should reap dead run and succeed
if "${WAKE}" --clean; then
  echo "PASS: --clean succeeded after reaping dead run"
else
  echo "FAIL: --clean should have succeeded after reap"
  exit 1
fi
