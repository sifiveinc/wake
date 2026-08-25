#! /bin/sh
# Failed jobs with no outputs must survive end-of-run cleanup, so they can still
# be debugged. Successful no-output jobs must still be reclaimed (they have no
# reuse value, and retaining them grows the database without bound).

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  rm -rf wake.db* wake.log .wake .build
}
trap cleanup EXIT
cleanup

# Run 1: command failure, runner failure, and a succeeding no-output job.
"${WAKE}" -q -x 'failingNoOutput Unit' >/dev/null
"${WAKE}" -q -x 'runnerFailNoOutput "initial"' >/dev/null
"${WAKE}" -q -x 'passingNoOutput Unit' >/dev/null
"${WAKE}" -q -x 'passingNoOutputOther Unit' >/dev/null

# Run 5: an unrelated job, so the GC watermark advances past all runs above and
# cleanup becomes eligible to reap their successful no-output jobs.
"${WAKE}" -q -x 'other Unit' >/dev/null

if ! "${WAKE}" --label no-output-failed --simple 2>&1 | grep -q 'no-output-failed'; then
  echo "FAIL: failed no-output job was reaped from the database" >&2
  exit 1
fi

# It must also be reachable the way a user would actually look for it.
if ! "${WAKE}" --failed --simple 2>&1 | grep -q 'no-output-failed'; then
  echo "FAIL: failed no-output job not reported by --failed" >&2
  exit 1
fi

if ! "${WAKE}" --label no-output-runner-failed-initial --simple 2>&1 | grep -q 'no-output-runner-failed-initial'; then
  echo "FAIL: runner-failed no-output job was reaped from the database" >&2
  exit 1
fi

if ! "${WAKE}" --failed --simple 2>&1 | grep -q 'no-output-runner-failed-initial'; then
  echo "FAIL: runner-failed no-output job not reported by --failed" >&2
  exit 1
fi

if "${WAKE}" --label no-output-passed --simple 2>&1 | grep -q 'no-output-passed'; then
  echo "FAIL: successful no-output job was retained; DB would grow unbounded" >&2
  exit 1
fi

if "${WAKE}" --label no-output-passed-other --simple 2>&1 | grep -q 'no-output-passed-other'; then
  echo "FAIL: second successful no-output job was retained; DB would grow unbounded" >&2
  exit 1
fi

echo "PASS: failed no-output job retained, successful one reclaimed" >&2
