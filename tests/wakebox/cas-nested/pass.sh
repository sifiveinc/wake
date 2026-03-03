#!/bin/sh
# Test that deeply nested directory structures work in CAS mode.
# Verifies that:
# 1. Multiple levels of directories can be created
# 2. Files at each level are accessible
# 3. Directory traversal (find) works correctly

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -rf a root.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

# Check that all levels are readable and find works
if echo "$OUTPUT" | grep -q "level0" && \
   echo "$OUTPUT" | grep -q "level1" && \
   echo "$OUTPUT" | grep -q "level2" && \
   echo "$OUTPUT" | grep -q "level3" && \
   echo "$OUTPUT" | grep -q "level4" && \
   echo "$OUTPUT" | grep -q "a/b/c/d/file4.txt"; then
    echo "PASS: Nested directory test succeeded"
    exit 0
else
    echo "FAIL: Nested directory test failed"
    echo "Output:"
    echo "$OUTPUT"
    exit 1
fi

