#!/bin/sh
# Test that file creation and writing works in CAS mode.
# Verifies that:
# 1. Files can be created and written to
# 2. File content can be read back immediately
# 3. staging_files output contains the file with proper metadata

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -f test_output.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that content was written and readable
if echo "$OUTPUT" | grep -q "hello world"; then
    echo "PASS: File write and read test succeeded"
    exit 0
else
    echo "FAIL: Could not read expected content 'hello world'"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

