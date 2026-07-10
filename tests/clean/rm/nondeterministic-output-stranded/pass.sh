#!/bin/sh
# Test: files with non-deterministic names left by a superseded job execution are
# physically removed (not stranded) so that `wake --clean` leaves a pristine workspace.
#
# Regression test for the leftover-file bug: when a keep=1 job re-executes with a
# divergently-named output (e.g. VC-Formal scratch named with a pid/timestamp), wake
# dropped the prior execution's output-file records via delete_prior without unlinking
# the physical files. `wake --clean` only removes files it still has records for, so the
# old file survived, failing pristine-workspace gates. The fix unlinks such stranded
# files when their records are dropped.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"
WAKE="$(command -v "$WAKE" 2>/dev/null || realpath "$WAKE")"

# Run in an isolated workspace so the job is independent of the repo's wake tree.
WS="$(mktemp -d)"
cleanup() {
    # wake's default (fuse) runner may leave a mount under .fuse; unmount best-effort before rm.
    if [ -d "$WS/.fuse" ]; then
        fusermount -u "$WS"/.fuse/* 2>/dev/null || true
        umount "$WS"/.fuse/* 2>/dev/null || true
    fi
    rm -rf "$WS" 2>/dev/null || true
}
trap cleanup EXIT

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

cp test.wake "$WS/build.wake"
echo '{}' > "$WS/.wakeroot"
cd "$WS"

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
