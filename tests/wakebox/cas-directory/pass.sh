#!/bin/sh
# Test that directory creation with files works in CAS mode.
# Verifies that:
# 1. Directories can be created
# 2. Files can be created inside directories
# 3. Directory listing works correctly
# 4. output_dir appears in staging_files with type "directory"
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

STATS_FILE=$(mktemp)
trap 'rm -rf output_dir "$STATS_FILE"' EXIT

OUTPUT=$(${1}/wakebox -o "$STATS_FILE" -p input.json 2>&1)

echo "$OUTPUT" | grep -q "file in dir" || { echo "FAIL: file content missing"; echo "$OUTPUT"; exit 1; }
echo "$OUTPUT" | grep -q "file.txt"    || { echo "FAIL: file.txt missing in ls output"; echo "$OUTPUT"; exit 1; }

grep -q '"output_dir"'       "$STATS_FILE" || { echo "FAIL: output_dir missing from staging_files"; cat "$STATS_FILE"; exit 1; }
grep -q '"type":"directory"' "$STATS_FILE" || { echo "FAIL: output_dir type is not directory"; cat "$STATS_FILE"; exit 1; }

echo "PASS"
