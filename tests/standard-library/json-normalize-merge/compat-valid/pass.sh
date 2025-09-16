#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

ln -f ../json-test.wake .
"${WAKE:-wake}" --quiet --stdout=warning,report 'jsonTest normalizeJSONCompat' input.json
rm json-test.wake
rm -r build
