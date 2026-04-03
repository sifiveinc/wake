#!/bin/sh
# It's not valid to call wakebox with an empty PATH.
# So we fill PATH with some typical values.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

trap 'rm -f link_file_src.txt link_file_dst.txt' EXIT

# Expect an error like "permission denied".
# Not presently checked for explicitly due to variations.
${1}/wakebox -p input.json
