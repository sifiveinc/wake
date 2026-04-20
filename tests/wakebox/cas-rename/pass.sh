#!/bin/sh
# Test that file and directory renaming works in CAS mode.
# Verifies that:
# 1. Files can be renamed and remain readable
# 2. Directories can be renamed
# 3. Files inside renamed directories are accessible with new paths
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -rf renamed.txt renamed_dir' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)

echo "$OUTPUT" | grep -q "rename test" || { echo "FAIL: 'rename test' missing from output"; echo "$OUTPUT"; exit 1; }
echo "$OUTPUT" | grep -q "file in dir" || { echo "FAIL: 'file in dir' missing from output"; echo "$OUTPUT"; exit 1; }

echo "PASS"
