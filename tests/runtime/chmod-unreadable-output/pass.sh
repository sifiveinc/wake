#! /bin/sh
# A job creates an output and chmods it write-only (0200).
#
# The build must succeed: the staging file is opened for reading to hash it and to copy
# it into CAS, so without a S_IRUSR floor in wakefuse_chmod that read fails. The
# materialized workspace file must still have the 0200 the job asked for -- the floor
# applies only to the staging file, never to what wake records or materializes.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

# output.txt should be removed since its mode 0200
trap 'rm -f output.txt' EXIT

rm -rf .build .fuse wake.db* wake.log output.txt

"${WAKE}" -q --no-tty -x 'test Unit'

echo "mode=$(ls -l output.txt | cut -c1-10)"

# Re-run from cache to confirm the blob was ingested and materializes the same way.
rm -f output.txt
"${WAKE}" -q --no-tty -x 'test Unit'

echo "cached mode=$(ls -l output.txt | cut -c1-10)"
