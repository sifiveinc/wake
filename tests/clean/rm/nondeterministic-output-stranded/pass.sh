#!/bin/sh
# Test: files with non-deterministic names left by a superseded job execution are
# physically removed (not stranded) so that `wake --clean` leaves a pristine workspace.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build outdir input.txt

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# First execution: MARKER=A -> the job writes outdir/f.<timestamp1>.txt
MARKER=A "${WAKE}" -q --no-tty -x 'go Unit' >/dev/null

count1=$(ls outdir | wc -l | tr -d ' ')
test "$count1" = "1" || fail "expected 1 output file after first run, got $count1"

# Change the job's input (via MARKER) so it cannot be reused and must re-execute,
# producing a divergently-named outdir/f.<timestamp2>.txt. This supersedes the prior job.
MARKER=B "${WAKE}" -q --no-tty -x 'go Unit' >/dev/null

# Now wake --clean should remove *all* wake-created output, leaving nothing behind.
"${WAKE}" --clean >/dev/null

if [ -d outdir ] && [ -n "$(ls -A outdir 2>/dev/null)" ]; then
    echo "FAIL: files stranded after wake --clean:" >&2
    ls -A outdir >&2
    exit 1
fi

echo "PASS: non-deterministic output not stranded after clean" >&2

rm -rf wake.db* wake.log .wake .build outdir input.txt
