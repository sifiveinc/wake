#! /bin/sh
# --last-used finds all jobs used by last run (including cached)
# --last-executed finds only jobs executed (not cached) in last run

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  rm -f wake.db* wake.log output.txt output2.txt
}
trap cleanup EXIT
cleanup

# Run 1: execute job1 (creates it)
"${WAKE}" -q -x 'testJob1 Unit'

# Verify job exists and --last finds it
if ! "${WAKE}" --last --simple 2>&1 | grep -q "last-used: test1"; then
  echo "FAIL: --last should find job1 after run 1"
  exit 1
fi

# Run 2: reuse job1 from cache AND create job2
"${WAKE}" -q -x 'testBoth Unit'

# --last-used should find BOTH jobs (job1 cached, job2 new)
if ! "${WAKE}" --last-used --simple 2>&1 | grep -q "last-used: test1"; then
  echo "FAIL: --last-used should find cached job1 in run 2"
  exit 1
fi
if ! "${WAKE}" --last-used --simple 2>&1 | grep -q "last-used: test2"; then
  echo "FAIL: --last-used should find new job2 in run 2"
  exit 1
fi

# --last-executed should only find job2 (created in run 2)
# job1 was cached, not executed
if "${WAKE}" --last-executed --simple 2>&1 | grep -q "last-used: test1"; then
  echo "FAIL: --last-executed should NOT find cached job1"
  exit 1
fi
if ! "${WAKE}" --last-executed --simple 2>&1 | grep -q "last-used: test2"; then
  echo "FAIL: --last-executed should find new job2"
  exit 1
fi
