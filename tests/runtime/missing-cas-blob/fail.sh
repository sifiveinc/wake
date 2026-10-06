#! /bin/sh

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS="output.txt"
cleanup() {
  rm -f $RM_ARTIFACTS
}
trap cleanup EXIT
rm -f wake.db* wake.log
cleanup

"${WAKE}" -q -x 'test Unit'

if [ ! -d .build/cas ]; then
  echo "CAS not located in expected location"
  exit 1
fi
rm .build/cas -rf

"${WAKE}" -q -x 'test Unit'
