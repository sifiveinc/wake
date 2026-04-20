#!/bin/sh
# Test that deeply nested directory structures work in CAS mode.
# Verifies that:
# 1. Multiple levels of directories can be created
# 2. Files at each level are accessible
# 3. Directory traversal (find) works correctly
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -rf a root.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)

for level in level0 level1 level2 level3 level4; do
    echo "$OUTPUT" | grep -q "$level" || { echo "FAIL: $level missing from output"; echo "$OUTPUT"; exit 1; }
done
echo "$OUTPUT" | grep -q "a/b/c/d/file4.txt" || { echo "FAIL: nested path missing from output"; echo "$OUTPUT"; exit 1; }

echo "PASS"
