#! /bin/sh
# Minor CAS GC test.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS="shared.txt"
rm -rf .build wake.db* wake.log $RM_ARTIFACTS

"${WAKE}" -q -x 'job2 Unit'
echo "After job2"
[ ! -e .build/cas/blobs/4a/89eb54e03e4e519e659ac83101df1e9773409425193964610cc52bee4fd06f ]
[ -e .build/cas/blobs/28/c2c99e8f8894c4d70f5d202056d9f3dc8bc920348137ab957eaa0e5e5cd79e ]

rm wake.db*
[ -e .build/cas/blobs/28/c2c99e8f8894c4d70f5d202056d9f3dc8bc920348137ab957eaa0e5e5cd79e ]
"${WAKE}" -q -x 'job1 Unit'
echo "After nuking DB and running job1"
[ ! -e .build/cas/blobs/28/c2c99e8f8894c4d70f5d202056d9f3dc8bc920348137ab957eaa0e5e5cd79e ]
[ -e .build/cas/blobs/4a/89eb54e03e4e519e659ac83101df1e9773409425193964610cc52bee4fd06f ]

# Clean up
rm -f $RM_ARTIFACTS
