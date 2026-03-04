#!/bin/sh
# Test for symlink vs file type confusion bug - bad job reuse (!!)
#
# 1. Create symlink input -> target, run wake (readlink returns "target")
# 2. Replace symlink with file containing "target" (same hash!)
# 3. Run wake again - readlink should FAIL but wake reuses cached "target"

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db .wake input

# Step 1: Create symlink and run wake
ln -sf target input
echo "=== Run 1: symlink ==="
RESULT1=$("${WAKE}" --no-tty -x 'test Unit' 2>&1)
echo "$RESULT1"

# Step 2: Replace symlink with file (same content = same hash)
rm -f input
printf 'target' > input

# Step 3: Run wake again
echo "=== Run 2: file ==="
RESULT2=$("${WAKE}" --no-tty -x 'test Unit' 2>&1)
echo "$RESULT2"

# Check: if both contain "readlink=target", wake reused (BUG)
# If run 2 contains "readlink=FAILED", wake correctly re-ran
if echo "$RESULT1" | grep -q "readlink=target" && echo "$RESULT2" | grep -q "readlink=target"; then
    echo ""
    echo "BUG CONFIRMED: wake reused readlink result after symlink->file change!"
    exit 1
fi

exit 0
