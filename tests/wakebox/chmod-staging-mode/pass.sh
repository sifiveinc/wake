#!/bin/sh
# Regression test: wakefuse_chmod must apply the mode to the on-disk staging file, not
# just track it in memory. Readers that bypass the FUSE daemon -- access(R_OK), wake-hash,
# CAS ingestion -- open that file directly and fail with Permission denied otherwise.
#
# The job in input.json creates two files with no read bit at all (install -m 0200):
#   restrictive.txt      then chmod'd 0644 -> staging file must end up 0644 on disk
#   stays_write_only.txt left at 0200      -> staging file must STILL be readable (the
#                                             S_IRUSR floor), while the mode wake tracks
#                                             and reports stays 0200
#
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

STATS_FILE=$(mktemp)
trap 'rm -f restrictive.txt stays_write_only.txt "$STATS_FILE"' EXIT

${1}/wakebox -o "$STATS_FILE" -p input.json

# Pull each file's staging_path out of the stats JSON.
staging_path_of() {
  tr ',' '\n' < "$STATS_FILE" \
    | grep -A2 "\"$1\"" \
    | grep -o '"staging_path":"[^"]*"' \
    | head -1 \
    | sed 's/.*:"//; s/"$//'
}

# chmod 0644 must have reached the on-disk staging file. Assert the exact mode.
rp=$(staging_path_of restrictive.txt)
[ -n "$rp" ] || { echo "FAIL: no staging_path for restrictive.txt"; cat "$STATS_FILE"; exit 1; }
rmode=$(stat -c '%a' "$rp")
echo "restrictive.txt staging mode=$rmode"
[ "$rmode" = "644" ] || { echo "FAIL: chmod 0644 did not reach staging file (got $rmode)"; exit 1; }

# The still-write-only file's staging file must be readable, even though the mode wake
# tracks and reports for it has no read bit. 0200 | S_IRUSR = 0600.
wp=$(staging_path_of stays_write_only.txt)
[ -n "$wp" ] || { echo "FAIL: no staging_path for stays_write_only.txt"; cat "$STATS_FILE"; exit 1; }
wmode=$(stat -c '%a' "$wp")
echo "stays_write_only.txt staging mode=$wmode"
[ -r "$wp" ] || { echo "FAIL: staging file $wp is not readable (S_IRUSR floor missing)"; exit 1; }

# ...and the tracked mode must still be the 0200 the job asked for (128 decimal),
# proving the floor is not leaking into what wake records.
grep -q '"mode":128' "$STATS_FILE" || { echo "FAIL: tracked mode for a 0200 file is not 128"; cat "$STATS_FILE"; exit 1; }

echo "PASS"
