#!/bin/sh
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

STATS_FILE=$(mktemp)
trap 'rm -rf output.txt "$STATS_FILE" .build/cas/staging/recovery' EXIT

"${1}/wakebox" -o "$STATS_FILE" -p input.json
MANIFEST=$(sed -n 's/.*"recovery_manifest":"\([^"]*\)".*/\1/p' "$STATS_FILE")
[ "${MANIFEST##*/}" = "run-42-job-7.json" ] || {
  echo "FAIL: unexpected Wake manifest name: $MANIFEST"
  exit 1
}
grep -q '"wake_run_id":42' "$MANIFEST" || { echo "FAIL: wake_run_id missing"; cat "$MANIFEST"; exit 1; }
grep -q '"wake_job_id":7' "$MANIFEST" || { echo "FAIL: wake_job_id missing"; cat "$MANIFEST"; exit 1; }

if "${1}/wakebox" -p unpaired-input.json >/dev/null 2>&1; then
  echo "FAIL: accepted an unpaired Wake identity"
  exit 1
fi

echo PASS
