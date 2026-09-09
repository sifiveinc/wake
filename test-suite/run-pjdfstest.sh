#!/usr/bin/env bash
set -euo pipefail

# Mount Wake's FUSE filesystem once and run pjdfstest inside that mount.
# Arguments:
#   $1 = mountpoint to create/use inside the sandbox
#   $2 = backing directory passed through mount.fuse via source=
#   $3 = absolute path to the pjdfstest checkout
#   $@ = optional prove args or specific test directories/files
mountpoint=$1
backing=$2
suite_root=$3
shift 3

mkdir -p "$mountpoint" "$backing"
chmod 0777 "$backing"

# If a previous failed run left the mount behind, clear it before reusing the
# same mountpoint.
umount -l "$mountpoint" >/dev/null 2>&1 || true

cleanup() {
  umount -l "$mountpoint" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

mount -t fuse non1 "$mountpoint" -o source="$backing",allow_other,default_permissions
cd "$mountpoint"

# pjdfstest expects to run from inside the mounted filesystem. When no explicit
# targets are provided, run the full suite. Relative test paths are resolved
# against the checkout root so callers can say "tests/rename" instead of having
# to pass absolute paths.
targets=()
if [ "$#" -eq 0 ]; then
  targets+=("$suite_root/tests")
else
  for arg in "$@"; do
    case "$arg" in
      /*|-*)
        targets+=("$arg")
        ;;
      *)
        targets+=("$suite_root/$arg")
        ;;
    esac
  done
fi

prove -rv "${targets[@]}"
