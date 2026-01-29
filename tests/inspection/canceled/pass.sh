#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

# Start from any empty db every time for stable job ids
rm -f wake.db

# Use || true to ignore the expected non-0 return from timeout
timeout 3 ${WAKE} test || true

# Give the database a moment to ensure WAL is flushed
# This is important because wake might be killed mid-transaction
sleep 0.2

${WAKE} --canceled --simple
