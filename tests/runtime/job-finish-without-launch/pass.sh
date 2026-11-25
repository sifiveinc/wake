#!/bin/sh

set -ex

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -f wake.db wake.log
"${WAKE}" -v test

echo "prim_job_finish without prim_job_launch passed!"
