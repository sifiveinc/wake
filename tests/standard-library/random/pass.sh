#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

"${WAKE:-wake}" testRandom
