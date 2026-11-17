#!/bin/sh

set -e
WAKE="${1:+$1/wake}"

RM_ARTIFACTS=".build/tmp/disable_remote_cache"
rm -f "$RM_ARTIFACTS"
trap 'rm -f "$RM_ARTIFACTS"' EXIT

"${WAKE:-wake}" -x "test Unit"

# Verify the disable cache file exists
if [ ! -f "$RM_ARTIFACTS" ]; then
    echo "Expected disable cache file to exist at $RM_ARTIFACTS"
    exit 1
fi

if ! grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' "$RM_ARTIFACTS"; then
    echo "Expected a UUIDv4 in the disable cache file"
    exit 1
fi

first_id=$(cat "$RM_ARTIFACTS")

# A sentinel from an earlier invocation must not disable the current invocation.
"${WAKE:-wake}" --in remote_cache -x "guardRemoteCacheDisabled Unit"
"${WAKE:-wake}" -x "test Unit"

if [ "$(cat "$RM_ARTIFACTS")" = "$first_id" ]; then
    echo "Expected a new ID for a new Wake invocation"
    exit 1
fi
