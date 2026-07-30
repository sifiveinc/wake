#!/bin/sh
# Regression test: access(W_OK) on a visible directory must agree with actual writability
# (pre-fix, it always denied W_OK even though creating files there worked).
#
# Verifies that:
# 1. `test -w foo` reports WRITABLE (access(2) no longer lies)
# 2. a new file can actually be created under the visible dir (CREATE_OK)
# 3. the created file is captured in staging_files
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

STATS_FILE=$(mktemp)
trap 'rm -f "$STATS_FILE"' EXIT

OUTPUT=$(${1}/wakebox -o "$STATS_FILE" -p input.json 2>&1)

# Anchored: must be "WRITABLE", not the "NOTWRITABLE" that the pre-fix code produced.
echo "$OUTPUT" | grep -qx "WRITABLE"  || { echo "FAIL: visible dir not reported writable"; echo "$OUTPUT"; exit 1; }
echo "$OUTPUT" | grep -qx "CREATE_OK" || { echo "FAIL: could not create file in visible dir"; echo "$OUTPUT"; exit 1; }
echo "$OUTPUT" | grep -qx "hi"        || { echo "FAIL: created file content missing"; echo "$OUTPUT"; exit 1; }

grep -q '"foo/new.txt"' "$STATS_FILE" || { echo "FAIL: foo/new.txt missing from staging_files"; cat "$STATS_FILE"; exit 1; }

echo "PASS"
