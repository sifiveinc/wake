#!/bin/sh
# Test for negative input tracking bug - bad job reuse (!!)

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db .wake bar

# 1. Run job testing for bar (doesn't exist)
echo "=== Run 1: bar doesn't exist ==="
RESULT1=$("${WAKE}" --no-tty -x 'test_nobar Unit' 2>&1)
echo "$RESULT1"

# 2. Create bar file
echo "bar" > bar

# 3. Run job testing for bar with bar visible
echo "=== Run 2: bar exists ==="
RESULT2=$("${WAKE}" --no-tty -x 'test_bar Unit' 2>&1)
echo "$RESULT2"

# Check: if both contain "bar exists: no", wake reused (BUG)
# If run 2 contains "bar exists: yes", wake correctly re-ran
if echo "$RESULT1" | grep -q "bar exists: no" && echo "$RESULT2" | grep -q "bar exists: no"; then
    echo ""
    echo "BUG CONFIRMED: wake reused 'no' result after bar was created!"
    exit 1
fi

exit 0

