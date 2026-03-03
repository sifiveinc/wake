#!/bin/sh
# Test that hardlinks work correctly in CAS mode.
# In CAS mode, hardlinks share the same staging_path, ensuring
# they get the same hash during processing.
# Verifies that:
# 1. Hardlinks can be created to staged files
# 2. Content is readable through both paths
# 3. Changes to original are visible through hardlink

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -f original.txt hardlink.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that both files show expected content
# After append, hardlink should show both lines
if echo "$OUTPUT" | grep -q "shared content" && \
   echo "$OUTPUT" | grep -q "appended"; then
    echo "PASS: Hardlink test succeeded"
    exit 0
else
    echo "FAIL: Hardlink test failed"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

