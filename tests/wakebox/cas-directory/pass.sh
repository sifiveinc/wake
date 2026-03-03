#!/bin/sh
# Test that directory creation with files works in CAS mode.
# Verifies that:
# 1. Directories can be created
# 2. Files can be created inside directories
# 3. Directory listing works correctly

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -rf output_dir' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that directory was created and file is readable
if echo "$OUTPUT" | grep -q "file in dir" && \
   echo "$OUTPUT" | grep -q "file.txt"; then
    echo "PASS: Directory creation test succeeded"
    exit 0
else
    echo "FAIL: Directory creation test failed"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

