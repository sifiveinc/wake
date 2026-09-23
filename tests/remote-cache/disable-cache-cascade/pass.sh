#!/bin/sh

set -e
WAKE="${1:+$1/wake}"

RM_ARTIFACTS=".build/tmp/disable_remote_cache"
rm -f $RM_ARTIFACTS

"${WAKE:-wake}" -x "test Unit"

# Verify the disable cache file exists
if [ ! -f .build/tmp/disable_remote_cache ]; then
    echo "Expected disable cache file to exist at .build/tmp/disable_remote_cache"
    exit 1
fi

# Clean up
rm -f $RM_ARTIFACTS
