#!/bin/sh
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

STATS_FILE=$(mktemp)
EXEC_DIR=
cleanup() {
    rm -f "$STATS_FILE"
    if [ -n "$EXEC_DIR" ]; then
        rm -f "$EXEC_DIR/1-output.txt" "$EXEC_DIR/recovery.json"
        rmdir "$EXEC_DIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"${1}/wakebox" -o "$STATS_FILE" -p input.json
MANIFEST=$(sed -n 's/.*"recovery_manifest":"\([^"]*\)".*/\1/p' "$STATS_FILE")
echo "$MANIFEST" | grep -q '^\.build/cas/staging/job-7-execution-[0-9a-f]\{32\}/recovery\.json$' || {
  echo "FAIL: unexpected Wake manifest location: $MANIFEST"
  exit 1
}
EXEC_DIR=${MANIFEST%/recovery.json}
grep -q '"wake_run_id":42' "$MANIFEST" || { echo "FAIL: wake_run_id missing"; cat "$MANIFEST"; exit 1; }
grep -q '"wake_job_id":7' "$MANIFEST" || { echo "FAIL: wake_job_id missing"; cat "$MANIFEST"; exit 1; }

if "${1}/wakebox" -p unpaired-input.json >/dev/null 2>&1; then
  echo "FAIL: accepted an unpaired Wake identity"
  exit 1
fi

echo PASS
