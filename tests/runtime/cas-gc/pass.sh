#! /bin/sh
# Minor CAS GC test.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf .build wake.db* wake.log shared.txt

"${WAKE}" -q -x 'job2 Unit'
echo "After job2"
[ ! -e .build/cas/blobs/aa/5cd7c969cdd10dc5788d1f72c92ec4540655325f17ee2cee8c8531fe836b40 ]
[ -e .build/cas/blobs/9f/5e6365d73eb89106c29f314d72b5b1e0d3b3a37598d3ada8f38b58532ecf77 ]

rm wake.db*
[ -e .build/cas/blobs/9f/5e6365d73eb89106c29f314d72b5b1e0d3b3a37598d3ada8f38b58532ecf77 ]
"${WAKE}" -q -x 'job1 Unit'
echo "After nuking DB and running job1"
[ ! -e .build/cas/blobs/9f/5e6365d73eb89106c29f314d72b5b1e0d3b3a37598d3ada8f38b58532ecf77 ]
[ -e .build/cas/blobs/aa/5cd7c969cdd10dc5788d1f72c92ec4540655325f17ee2cee8c8531fe836b40 ]
