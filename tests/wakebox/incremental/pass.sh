#!/bin/bash

# Test script for runners supporting incremental compilation.
# This script tests that incremental compilation works correctly:
# - After initial compilation, only changed files and their dependents should be recompiled.
# - Files that haven't changed should keep their original timestamps as a basic sanity check.

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

echo "=== Testing incremental compilation ==="

# Clean up any previous build artifacts; this should have already been done, but it pays to be sure.
cleanup() {
    if [[ -f main.c.orig ]]; then
        mv main.c.orig main.c
    fi
    # TODO: This should use `wake --clean`, but a quick test doesn't seem to remove anything.
    rm -f impl.o main.o hello
}
cleanup

echo "Step 1: Initial compilation with wake test"
"$WAKE" test

# Verify that all intermediate and output files were created.
if [[ ! -f impl.o || ! -f main.o || ! -f hello ]]; then
    echo "ERROR: Initial compilation failed - missing output files"
    cleanup
    exit 1
fi

echo "Step 2: Recording initial timestamps"
# The `format` flag differs between Linux and macOS/BSD respectively.
impl_o_time1=$(stat -c %Y impl.o 2>/dev/null || stat -f %m impl.o)
main_o_time1=$(stat -c %Y main.o 2>/dev/null || stat -f %m main.o)
hello_time1=$(stat -c %Y hello 2>/dev/null || stat -f %m hello)

echo "Initial timestamps:"
echo "  impl.o: $impl_o_time1"
echo "  main.o: $main_o_time1"
echo "  hello: $hello_time1"

echo "Step 3: Making a small change to main.c"
cp --preserve main.c main.c.orig
echo "// Incremental compilation test change" >> main.c

echo "Step 4: Second compilation with wake test"
# Wait a moment to ensure timestamp differences.  The %Y resolution is in seconds, and the `wake`
# overhead should add enough padding to avoid rounding errors.
sleep 1
"$WAKE" test

# Verify files still exist just in case something *very* weird happened.
if [[ ! -f impl.o || ! -f main.o || ! -f hello ]]; then
    echo "ERROR: Second compilation failed - missing output files"
    cleanup
    exit 1
fi

echo "Step 5: Recording new timestamps"
impl_o_time2=$(stat -c %Y impl.o 2>/dev/null || stat -f %m impl.o)
main_o_time2=$(stat -c %Y main.o 2>/dev/null || stat -f %m main.o)
hello_time2=$(stat -c %Y hello 2>/dev/null || stat -f %m hello)

echo "New timestamps:"
echo "  impl.o: $impl_o_time2"
echo "  main.o: $main_o_time2"
echo "  hello: $hello_time2"

echo "Step 6: Verifying incremental compilation behavior"

# Check that main.o and the final binary were updated (newer timestamps).
if [[ $main_o_time1 -ge $main_o_time2 ]]; then
    echo "ERROR: main.o was not updated after main.c change"
    echo "  Expected: $main_o_time1 < $main_o_time2"
    cleanup
    exit 1
fi

if [[ $hello_time1 -ge $hello_time2 ]]; then
    echo "ERROR: hello binary was not updated after main.c change"
    echo "  Expected: $hello_time1 < $hello_time2"
    cleanup
    exit 1
fi

# Check that impl.o was NOT updated (same timestamp).
if [[ $impl_o_time1 -ne $impl_o_time2 ]]; then
    echo "ERROR: impl.o was unnecessarily recompiled"
    echo "  Expected: $impl_o_time1 == $impl_o_time2"
    echo "  Actual: $impl_o_time1 != $impl_o_time2"
    cleanup
    exit 1
fi

echo "SUCCESS: Incremental compilation working correctly!"
echo "  ✓ main.o was recompiled"
echo "  ✓ hello binary was updated"
echo "  ✓ impl.o was not unnecessarily recompiled"

cleanup

echo "=== Test completed successfully ==="
