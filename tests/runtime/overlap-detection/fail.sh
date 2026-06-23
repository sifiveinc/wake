#! /bin/sh
# Two jobs output same file - wake should fail with overlap error

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -f overlap.txt wake.db wake.log
"${WAKE}" -v test

# Clean up
err=$?
rm -f .wake-build-lock .wake/locks/* overlap.txt
exit $err
