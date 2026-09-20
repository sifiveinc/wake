#!/bin/sh
# Test that file creation and writing works in CAS mode.
# Verifies that:
# 1. Files can be created and written to
# 2. File content can be read back immediately
# 3. staging_files output contains the file with type "file" and a staging_path
# 4. the result hands off an atomically written final recovery manifest
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

STATS_FILE=$(mktemp)
trap 'rm -rf test_output.txt "$STATS_FILE" .build/cas/staging/recovery' EXIT

OUTPUT=$(${1}/wakebox -o "$STATS_FILE" -p input.json 2>&1)

echo "$OUTPUT" | grep -q "hello world" || { echo "FAIL: expected 'hello world' in output"; echo "$OUTPUT"; exit 1; }

grep -q '"test_output.txt"' "$STATS_FILE" || { echo "FAIL: test_output.txt missing from staging_files"; cat "$STATS_FILE"; exit 1; }
grep -q '"type":"file"'     "$STATS_FILE" || { echo "FAIL: test_output.txt type is not file"; cat "$STATS_FILE"; exit 1; }
grep -q '"staging_path"'    "$STATS_FILE" || { echo "FAIL: staging_path missing from staging_files"; cat "$STATS_FILE"; exit 1; }
MANIFEST=$(sed -n 's/.*"recovery_manifest":"\([^"]*\)".*/\1/p' "$STATS_FILE")
[ -n "$MANIFEST" ] || { echo "FAIL: recovery manifest missing from result"; cat "$STATS_FILE"; exit 1; }
[ -f "$MANIFEST" ] || { echo "FAIL: recovery manifest was not written"; exit 1; }
grep -q '"version":1' "$MANIFEST" || { echo "FAIL: recovery manifest version missing"; cat "$MANIFEST"; exit 1; }
grep -q '"destination":"test_output.txt"' "$MANIFEST" || { echo "FAIL: recovery manifest output missing"; cat "$MANIFEST"; exit 1; }
case "$MANIFEST" in
  */wakebox-*-*.json) ;;
  *) echo "FAIL: standalone manifest has unexpected name: $MANIFEST"; exit 1 ;;
esac
! grep -q '"wake_run_id"' "$MANIFEST" || { echo "FAIL: standalone manifest contains Wake IDs"; cat "$MANIFEST"; exit 1; }

echo "PASS"
