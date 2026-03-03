#!/bin/sh
# Test that symlink creation works in CAS mode.
# Verifies that:
# 1. Symlinks can be created pointing to staged files
# 2. Reading through symlink returns correct content
# 3. readlink returns the correct target

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -f target.txt symlink.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that symlink content is readable and readlink works
if echo "$OUTPUT" | grep -q "target content" && \
   echo "$OUTPUT" | grep -q "target.txt"; then
    echo "PASS: Symlink creation and read test succeeded"
    exit 0
else
    echo "FAIL: Symlink test failed"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

