#! /bin/sh
# Test: wake-migrate does not create the database.
set -eu

MIGRATE="${1:+$1/wake-migrate}"
MIGRATE="${MIGRATE:-wake-migrate}"
cd "$(dirname "$0")"

rm -f wake.db

"$MIGRATE" wake.db
