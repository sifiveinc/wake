#! /bin/sh

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  rm -f wake.db* wake.log output.txt
}
trap cleanup EXIT
cleanup

export WAKE_CAS=1
"${WAKE}" -q -x 'test Unit'

if [ ! -d .build/cas ]; then
  echo "CAS not located in expected location"
  exit 1
fi
rm .build/cas -rf

"${WAKE}" -q -x 'test Unit'

