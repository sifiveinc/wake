#! /bin/sh

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf x y
rm -f wake.db wake.log

"${WAKE}" -q -x 'test Unit'

"${WAKE}" --clean

# There should not be files leftover!
if [ -e x/z ]; then
  echo "File leftover in x/z"
  exit 1
fi
