#!/bin/sh
# Test that symlink creation works in CAS mode.
# Verifies that:
# 1. Symlinks can be created pointing to staged files
# 2. Reading through symlink returns correct content
# 3. readlink returns the correct target
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -f target.txt symlink.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)

echo "$OUTPUT" | grep -q "target content" || { echo "FAIL: 'target content' missing from output"; echo "$OUTPUT"; exit 1; }
echo "$OUTPUT" | grep -q "target.txt"     || { echo "FAIL: readlink target missing from output"; echo "$OUTPUT"; exit 1; }

echo "PASS"
