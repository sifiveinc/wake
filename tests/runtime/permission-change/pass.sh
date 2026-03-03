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
"${WAKE}" --no-tty -x 'test Unit'

# Step 2: Make it executable (same content = same hash!)
chmod +x script
touch script

# Step 3: Run wake again
echo "=== Run 2: executable ==="
"${WAKE}" --no-tty -x 'test Unit'
