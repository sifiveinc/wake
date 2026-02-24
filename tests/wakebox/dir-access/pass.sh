#!/bin/sh
# Test that access() checks work correctly on staged directories.
# This tests the fix for the bug where wakefuse_access() would call
# access() on an empty staging_path for staged directories, causing
# access checks to fail even though the directory was valid.
#
# VCS (and other tools) use access() to check if directories are
# readable/writable before using them.

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

# Clean up any leftover files from previous runs
trap 'rm -rf testdir' EXIT

STDOUT=$(${1}/wakebox -p input.json 2>&1)
EXIT_CODE=$?

# Check that the command succeeded
if [ "$EXIT_CODE" != "0" ]; then
    echo "FAIL: Command failed with exit code $EXIT_CODE"
    echo "Output:"
    echo "$STDOUT"
    exit 1
fi

# Check that all access checks passed
if echo "$STDOUT" | grep -q "READ_OK" && \
   echo "$STDOUT" | grep -q "WRITE_OK" && \
   echo "$STDOUT" | grep -q "EXEC_OK" && \
   echo "$STDOUT" | grep -q "IS_DIR" && \
   echo "$STDOUT" | grep -q "test content" && \
   echo "$STDOUT" | grep -q "ACCESS_TEST_PASSED"; then
    echo "PASS: Directory access test succeeded"
    exit 0
else
    echo "FAIL: Access checks failed on staged directory"
    echo "Output:"
    echo "$STDOUT"
    exit 1
fi

