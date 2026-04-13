#!/bin/sh
# Test for negative input tracking bug - bad job reuse (!!)

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db .wake bar

# 1. Run job testing for bar (doesn't exist)
echo "=== Run 1: bar doesn't exist ==="
"${WAKE}" --no-tty -x 'test_nobar Unit'

# 2. Create bar file
echo "bar" > bar

# 3. Run job testing for bar with bar visible
echo "=== Run 2: bar exists ==="
"${WAKE}" --no-tty -x 'test_bar Unit'
