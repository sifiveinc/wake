#!/bin/sh
# Verify that utimens after multiple writes produces the correct mtime in both
# the FUSE mount (observed via the job's stdout/getattr) and the materialized
# workspace file (observed via the JSON emission path).
#
# The bug this catches: mtime was captured at first release and stale metadata
# was emitted in the output JSON, so the workspace file got the mtime from the
# first write instead of the explicitly-set value.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf .build .fuse wake.db* wake.log output.txt

"${WAKE}" -x "go Unit"

# Verify the workspace file mtime matches what was set inside the job.
# This catches bugs in JSON emission: if the wrong mtime is serialized, wakebox
# materializes the workspace file with the wrong mtime even though getattr inside
# the FUSE mount was correct (getattr always stats the backing file directly).
actual=$(date -Isec -ur output.txt)
expected="2000-01-01T00:00:00+00:00"
if [ "$actual" != "$expected" ]; then
    echo "FAIL: workspace output.txt mtime is '$actual', expected '$expected'" >&2
    exit 1
fi
