#!/bin/sh
# Test that file and directory renaming works in CAS mode.
# Verifies that:
# 1. Files can be renamed and remain readable
# 2. Directories can be renamed
# 3. Files inside renamed directories are accessible with new paths

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -rf renamed.txt renamed_dir' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that both file and directory rename worked
if echo "$OUTPUT" | grep -q "rename test" && \
   echo "$OUTPUT" | grep -q "file in dir"; then
    echo "PASS: Rename test succeeded"
    exit 0
else
    echo "FAIL: Rename test failed"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

