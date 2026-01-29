#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

# Start from any empty db every time for stable job ids
rm -f wake.db

# Use || true to ignore the expected non-0 return from timeout
timeout 3 ${WAKE} test || true


${WAKE} --canceled --simple
