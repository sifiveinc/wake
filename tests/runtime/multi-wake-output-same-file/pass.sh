#! /bin/sh
# Allow jobs to output the "same" file; disallow if contents or details are different.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS=".wake/locks/* shared.txt err.txt"
rm -f wake.db* wake.log $RM_ARTIFACTS

"${WAKE}" -q -x 'test_same Unit'

! "${WAKE}" -q -x 'test_diff Unit' > err.txt

sed 's/job \([0-9]\+\)/job -/' err.txt

# Clean up
rm -f $RM_ARTIFACTS
