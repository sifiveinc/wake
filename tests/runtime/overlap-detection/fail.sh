#! /bin/sh
# Two jobs output same file - wake should fail with overlap error

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS=".wake/locks/* overlap.txt"
rm -f wake.db wake.log $RM_ARTIFACTS
"${WAKE}" -v test

# Clean up
err=$?
rm -f $RM_ARTIFACTS
exit $err
