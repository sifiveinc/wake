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
"${WAKE}" --no-tty -x 'test Unit'

# Step 2: Replace symlink with file (same content = same hash)
rm -f input
printf 'target' > input

# Step 3: Run wake again
echo "=== Run 2: file ==="
"${WAKE}" --no-tty -x 'test Unit'
