#!/bin/sh
# Test --checkout-to with --leaves: with --leaves, only outputs not consumed by
# another job in the same run are materialized.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  rm -rf wake.db* wake.log .wake .build all leaves intermediate.txt final.txt
}
trap cleanup EXIT
cleanup

# 2-job pipeline: job-a -> intermediate.txt -> job-b -> final.txt.
"${WAKE}" -x 'build Unit' >/dev/null

# Without --leaves: both intermediate.txt and final.txt are checked out.
echo "== --checkout-to (no --leaves) =="
"${WAKE}" --checkout-to all
(cd all && find . -mindepth 1 | sort)

# With --leaves: intermediate.txt is consumed by job-b so it's filtered out.
echo "== --checkout-to --leaves =="
"${WAKE}" --checkout-to leaves --leaves
(cd leaves && find . -mindepth 1 | sort)
