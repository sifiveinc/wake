#!/bin/sh
# A staged file can inherit a setgid CAS group that Wakebox does not map into
# its user namespace. FUSE must report the mapped daemon identity instead.
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

primary_gid=$(id -g)
setgid_gid=
for gid in $(id -G); do
  if [ "$gid" != "$primary_gid" ]; then
    setgid_gid=$gid
    break
  fi
done

# Some CI environments have no supplementary group available for this test.
if [ -z "$setgid_gid" ]; then
  echo "SKIP: set a supplementary GID to run this test"
  exit 0
fi

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=$(cd "$test_dir" && {
  mkdir -p .build/cas/staging
  chgrp "$setgid_gid" .build/cas .build/cas/staging
  chmod 2775 .build/cas .build/cas/staging
  "${1}/wakebox" -p input.json
} 2>&1)
echo "$output" | grep -qx '0:0' || {
  echo "FAIL: staged file owner is not mapped in Wakebox"
  echo "$output"
  exit 1
}

echo "PASS"
