#! /bin/sh
# Allow jobs to output the "same" file; disallow if contents or details are different.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -f wake.db* wake.log shared.txt err.txt

"${WAKE}" -q -x 'test_same Unit'

! "${WAKE}" -q -x 'test_diff Unit' > err.txt

sed 's/job \([0-9]\+\)/job -/' err.txt
