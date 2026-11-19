#!/bin/sh
set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"
WAKE=$(command -v "$WAKE")
WAKE="$(cd "$(dirname "$WAKE")" && pwd)/$(basename "$WAKE")"
FIXTURE="$(pwd)/test.wake"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/wake-cas-long-output-name.XXXXXXXX")
WORK=$(cd "$WORK" && pwd)
cleanup() {
    # Do not recursively remove a FUSE mount that is still shutting down.
    for mount in "$WORK"/.fuse/*; do
        [ -d "$mount" ] || continue
        for _ in $(seq 1 100); do
            rmdir "$mount" 2>/dev/null && break
            sleep 0.1
        done
        if [ -d "$mount" ]; then
            echo "FAIL: FUSE mount did not shut down; leaving $WORK" >&2
            exit 1
        fi
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
cp "$FIXTURE" "$WORK/test.wake"
cd "$WORK"
name=$(printf '%0251d' 0)

"$WAKE" --init .
"$WAKE" -q --no-tty -x 'test Unit'
[ "$(cat "$name")" = contents ]

echo PASS
