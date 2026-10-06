#!/bin/sh
# A failed job with no outputs must survive the next Wake invocation's GC pass.

set -u

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS="unrelated.txt"
cleanup() {
  rm -rf .wake wake.db* wake.log $RM_ARTIFACTS
}
trap cleanup EXIT
cleanup

# Run 1: the job fails and records no output rows.
"${WAKE}" -q failedNoOutput
status=$?
if [ "$status" -eq 0 ]; then
  echo "FAIL: failedNoOutput unexpectedly succeeded"
  exit 2
fi

"${WAKE}" --failed || true

# Run 2: while this run is active, the GC watermark is run_id - 1, making
# run 1 eligible for sql_delete_jobs.  This build is unrelated and succeeds.
"${WAKE}" -q unrelatedSuccess
status=$?
if [ "$status" -ne 0 ]; then
  echo "FAIL: unrelatedSuccess failed with status $status"
  exit 3
fi

"${WAKE}" --failed || true
