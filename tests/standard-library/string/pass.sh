#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

"${WAKE:-wake}" --stdout=warning,report testToBytes
"${WAKE:-wake}" --stdout=warning,report testToUnicode
"${WAKE:-wake}" --stdout=warning,report testEquality
