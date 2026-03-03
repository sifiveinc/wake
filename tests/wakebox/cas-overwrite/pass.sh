#!/bin/sh
# Test that file overwriting works correctly in CAS mode.
# When a file is overwritten, the old staging file should be deleted
# and a new one created.
# Verifies that:
# 1. File can be overwritten multiple times
# 2. Each read returns the current content
# 3. Final content is correct

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -f overwrite.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that we see version 1, 2, and 3 in output (showing progression)
if echo "$OUTPUT" | grep -q "version 1" && \
   echo "$OUTPUT" | grep -q "version 2" && \
   echo "$OUTPUT" | grep -q "version 3"; then
    echo "PASS: File overwrite test succeeded"
    exit 0
else
    echo "FAIL: File overwrite test failed"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

