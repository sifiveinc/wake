#!/bin/sh
# Test that file creation and writing works in CAS mode.
# Verifies that:
# 1. Files can be created and written to
# 2. File content can be read back immediately
# 3. staging_files output contains the file with type "file" and a staging_path
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

STATS_FILE=$(mktemp)
trap 'rm -f test_output.txt "$STATS_FILE"' EXIT

OUTPUT=$(${1}/wakebox -o "$STATS_FILE" -p input.json 2>&1)

echo "$OUTPUT" | grep -q "hello world" || { echo "FAIL: expected 'hello world' in output"; echo "$OUTPUT"; exit 1; }

grep -q '"test_output.txt"' "$STATS_FILE" || { echo "FAIL: test_output.txt missing from staging_files"; cat "$STATS_FILE"; exit 1; }
grep -q '"type":"file"'     "$STATS_FILE" || { echo "FAIL: test_output.txt type is not file"; cat "$STATS_FILE"; exit 1; }
grep -q '"staging_path"'    "$STATS_FILE" || { echo "FAIL: staging_path missing from staging_files"; cat "$STATS_FILE"; exit 1; }

echo "PASS"
