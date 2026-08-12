#!/bin/sh
# Exercises stagingDirRunner: a job runner that runs jobs against a private copy of
# their declared visible inputs (no FUSE) and stages declared outputs directly out of
# that private tree.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"


ARTIFACTS="basic-in.txt basic-out.txt \
    symlink-in.txt symlink-out.link \
    nested-out \
    isolation-secret.txt isolation-out.txt \
    filter-a.txt filter-b.txt filter-out.txt \
    failing-out.txt \
    does-not-exist.txt \
    filter-declared.txt"

cleanup() {
    rm -rf .build wake.db* wake.log $ARTIFACTS
}
trap cleanup EXIT
cleanup

"${WAKE}" -x 'test Unit'

# Rerun to check that staged outputs are stable enough for wake's normal cache-hit
# machinery: every successful job from the first run should be a pure cache hit (not
# re-executed) on an unchanged rerun. The one job that intentionally fails
# ("failing-with-output") is never cached and is expected to run again every time.
"${WAKE}" -x 'test Unit'

reexecuted=$("${WAKE}" --last-executed --simple | grep '^# staging-dir-runner:' || true)

unexpected=$(echo "$reexecuted" | grep -v '^# staging-dir-runner: failing-with-output' || true)

if [ -n "$unexpected" ]; then
    echo "FAIL: expected only 'failing-with-output' to re-execute on an unchanged rerun, but got:" >&2
    echo "$unexpected" >&2
    exit 1
fi

if ! echo "$reexecuted" | grep -q '^# staging-dir-runner: failing-with-output'; then
    echo "FAIL: expected 'failing-with-output' to re-execute (failed jobs are never cached)" >&2
    exit 1
fi
