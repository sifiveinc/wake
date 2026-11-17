#!/bin/sh
# Test that file creation and writing works in CAS mode.
# Verifies that:
# 1. Files can be created and written to
# 2. File content can be read back immediately
# 3. staging_files output contains the file with type "file" and a staging_path
# 4. the result hands off an atomically written final recovery manifest, stored beside the
#    execution's flat, numbered payloads in wakebox-exec-<UUID>/
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

STATS_FILE=$(mktemp)
EXEC_DIR=
cleanup() {
    rm -f "$STATS_FILE"
    if [ -n "$EXEC_DIR" ]; then
        rm -f "$EXEC_DIR/1-test_output.txt" "$EXEC_DIR/recovery.json"
        rmdir "$EXEC_DIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT

OUTPUT=$("${1}/wakebox" -o "$STATS_FILE" -p input.json 2>&1)

echo "$OUTPUT" | grep -q "hello world" || { echo "FAIL: expected 'hello world' in output"; echo "$OUTPUT"; exit 1; }

grep -q '"test_output.txt"' "$STATS_FILE" || { echo "FAIL: test_output.txt missing from staging_files"; cat "$STATS_FILE"; exit 1; }
grep -q '"type":"file"'     "$STATS_FILE" || { echo "FAIL: test_output.txt type is not file"; cat "$STATS_FILE"; exit 1; }
MANIFEST=$(sed -n 's/.*"recovery_manifest":"\([^"]*\)".*/\1/p' "$STATS_FILE")
[ -n "$MANIFEST" ] || { echo "FAIL: recovery manifest missing from result"; cat "$STATS_FILE"; exit 1; }
case "$MANIFEST" in
  .build/cas/staging/wakebox-exec-*/recovery.json) ;;
  *) echo "FAIL: unexpected recovery manifest location: $MANIFEST"; exit 1 ;;
esac
DIR_NAME=${MANIFEST#.build/cas/staging/wakebox-exec-}
DIR_NAME=${DIR_NAME%/recovery.json}
case "$DIR_NAME" in
  ''|*[!0-9a-f]*) echo "FAIL: unsafe execution directory: $MANIFEST"; exit 1 ;;
esac
EXEC_DIR=${MANIFEST%/recovery.json}
STAGING_PATH=$(sed -n 's/.*"staging_path":"\([^"]*\)".*/\1/p' "$STATS_FILE")
[ "$STAGING_PATH" = "$EXEC_DIR/1-test_output.txt" ] || { echo "FAIL: result staging_path '$STAGING_PATH' is not the payload beside $MANIFEST"; cat "$STATS_FILE"; exit 1; }
JOB_STAGING_ROOT=$(sed -n 's/.*"job_staging_root":"\([^"]*\)".*/\1/p' "$STATS_FILE")
[ "$JOB_STAGING_ROOT" = "$EXEC_DIR" ] || { echo "FAIL: job_staging_root '$JOB_STAGING_ROOT' is not the execution directory '$EXEC_DIR'"; cat "$STATS_FILE"; exit 1; }
[ -f "$MANIFEST" ] || { echo "FAIL: recovery manifest was not written"; exit 1; }
grep -q '"version":2' "$MANIFEST" || { echo "FAIL: recovery manifest version missing"; cat "$MANIFEST"; exit 1; }
grep -q '"destination":"test_output.txt"' "$MANIFEST" || { echo "FAIL: recovery manifest output missing"; cat "$MANIFEST"; exit 1; }
grep -q "\"staging_path\":\"${EXEC_DIR##*/}/1-test_output.txt\"" "$MANIFEST" || { echo "FAIL: manifest staging_path is not relative to the staging root"; cat "$MANIFEST"; exit 1; }
[ -f "$EXEC_DIR/1-test_output.txt" ] || { echo "FAIL: numbered payload is not beside the manifest"; ls -la "$EXEC_DIR"; exit 1; }
! grep -q '"wake_run_id"' "$MANIFEST" || { echo "FAIL: standalone manifest contains Wake IDs"; cat "$MANIFEST"; exit 1; }

echo "PASS"
