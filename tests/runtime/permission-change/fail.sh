#!/bin/sh
# Test for permission tracking bug/behavior.
#
# Essentially, permissions are not part of hash yet are observable.
#
# 1. Create non-executable script, run wake ("test -x" returns "no")
# 2. chmod +x the script (same content = same hash!)
# 3. Run wake again - "test -x" should return "yes" but wake reuses "no"

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db .wake script

# Step 1: Create non-executable file and run wake
echo "#!/bin/sh" > script
chmod -x script
echo "=== Run 1: non-executable ==="
RESULT1=$("${WAKE}" --no-tty -x 'test Unit' 2>&1)
echo "$RESULT1"

# Step 2: Make it executable (same content = same hash!)
chmod +x script
touch script

# Step 3: Run wake again
echo "=== Run 2: executable ==="
RESULT2=$("${WAKE}" --no-tty -x 'test Unit' 2>&1)
echo "$RESULT2"

# Check: if both contain "test-x=no", wake reused (BUG)
# If run 2 contains "test-x=yes", wake correctly re-ran
if echo "$RESULT1" | grep -q "test-x=no" && echo "$RESULT2" | grep -q "test-x=no"; then
    echo ""
    echo "BUG CONFIRMED: wake reused 'no' result after chmod +x!"
    exit 1
fi

exit 0

