#!/bin/sh
# Each Wakebox execution owns one job-<jobID>-execution-<UUID>/ directory below
# .build/cas/staging. Its regular payloads are flat, numbered, readable files (so equal
# basenames never collide) and recovery.json sits beside them. Repeated executions of the
# same Wake job get distinct directories.
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

WAKEBOX="$(cd "${1:?expected the Wake binary directory}" && pwd -P)/wakebox"
FIXTURES=$(pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/wakebox-job-staging.XXXXXXXX")
WORK=$(cd "$WORK" && pwd -P)
cleanup() {
    # The workspace-local FUSE daemon detaches after its idle timeout.
    marker="$WORK/.fuse/$(id -u).$(id -g)/.f.fuse-waked"
    attempts=0
    while [ -e "$marker" ]; do
        if [ "$attempts" -ge 100 ]; then
            echo "FAIL: FUSE daemon did not detach from $WORK" >&2
            exit 1
        fi
        sleep 0.1
        attempts=$((attempts + 1))
    done
    rm -rf "$WORK" || exit 1
}
trap cleanup EXIT
cd "$WORK"
STATS_A="$WORK/stats-a.json"
STATS_B="$WORK/stats-b.json"
STATS_EMPTY="$WORK/stats-empty.json"

fail() { echo "FAIL: $*"; exit 1; }
field() { sed -n "s/.*\"$1\":\"\\([^\"]*\\)\".*/\\1/p" "$2"; }

check_v2() {
    manifest="$1/recovery.json"
    grep -q '"version"[[:space:]]*:[[:space:]]*2[[:space:]]*[,}]' "$manifest" || fail "not a v2 manifest: $manifest"
    [ "$(field job_staging_dir "$manifest")" = "${1##*/}" ] || fail "wrong staging directory in $manifest"
    [ "$(field execution_uuid "$manifest")" = "${1##*-execution-}" ] || fail "wrong execution UUID in $manifest"
}

"$WAKEBOX" -o "$STATS_A" -p "$FIXTURES/input.json" >/dev/null
"$WAKEBOX" -o "$STATS_B" -p "$FIXTURES/input.json" >/dev/null

DIR_A=$(field job_staging_root "$STATS_A")
DIR_B=$(field job_staging_root "$STATS_B")
[ -n "$DIR_A" ] && [ -n "$DIR_B" ] || fail "job_staging_root missing from result"
[ "$DIR_A" != "$DIR_B" ] || fail "repeated executions of one job share a directory: $DIR_A"
[ "$(field recovery_manifest "$STATS_A")" = "$DIR_A/recovery.json" ] || fail "manifest is not beside payloads"
[ "$(field recovery_manifest "$STATS_B")" = "$DIR_B/recovery.json" ] || fail "manifest is not beside payloads"
for dir in "$DIR_A" "$DIR_B"; do
  echo "$dir" | grep -q '^\.build/cas/staging/job-8-execution-[0-9a-f]\{32\}$' || fail "unexpected directory name: $dir"
  [ -f "$dir/recovery.json" ] || fail "missing $dir/recovery.json"
  check_v2 "$dir"
done

# Payloads are flat numbered files; two outputs named main.o do not collide.
payloads=$(ls "$DIR_A" | grep -v '^recovery\.json$' | sort)
echo "$payloads" | grep -q '^[1-9][0-9]*-main\.o$' || fail "no unpadded numbered main.o payload: $payloads"
[ "$(echo "$payloads" | grep -c -- '-main\.o$')" = 2 ] || fail "expected two main.o payloads: $payloads"
[ -z "$(find "$DIR_A" -mindepth 1 -type d)" ] || fail "payload directory is not flat"

# An execution with no outputs still owns a directory and a manifest.
"$WAKEBOX" -o "$STATS_EMPTY" -p "$FIXTURES/empty-input.json" >/dev/null
DIR_E=$(field job_staging_root "$STATS_EMPTY")
[ -f "$DIR_E/recovery.json" ] || fail "empty execution has no manifest: $DIR_E"
[ "$(field recovery_manifest "$STATS_EMPTY")" = "$DIR_E/recovery.json" ] || fail "empty execution returned the wrong manifest"
[ "$(ls "$DIR_E")" = recovery.json ] || fail "empty execution has unexpected files"
check_v2 "$DIR_E"

# Exact recovery consumes only the named execution and removes its emptied directory.
FILES_B=$(cd "$DIR_B" && cksum *)
"$WAKEBOX" -M "$DIR_A/recovery.json" >/dev/null || fail "exact recovery failed"
[ "$(cat layout-a/main.o)" = one ] || fail "layout-a/main.o has wrong content"
[ "$(cat layout-b/main.o)" = two ] || fail "layout-b/main.o has wrong content"
[ ! -e "$DIR_A" ] || fail "recovered directory was not removed: $DIR_A"
[ -f "$DIR_B/recovery.json" ] || fail "recovery of one execution touched another"
[ "$(cd "$DIR_B" && cksum *)" = "$FILES_B" ] || fail "exact recovery changed another execution"

# Run-scoped recovery processes all matching manifests without rejecting repeated job IDs.
"$WAKEBOX" -o "$STATS_A" -p "$FIXTURES/input.json" >/dev/null
DIR_C=$(field job_staging_root "$STATS_A")
[ -n "$DIR_C" ] && [ -f "$DIR_C/recovery.json" ] || fail "third execution has no manifest"
[ "$DIR_C" != "$DIR_B" ] || fail "third execution reused another attempt's directory"
check_v2 "$DIR_C"
sed 's/"wake_run_id": 43/"wake_run_id": 44/' "$FIXTURES/input.json" >"$WORK/other-run.json"
"$WAKEBOX" -o "$WORK/stats-other.json" -p "$WORK/other-run.json" >/dev/null
DIR_OTHER=$(field job_staging_root "$WORK/stats-other.json")
[ -f "$DIR_OTHER/recovery.json" ] || fail "other run has no manifest"
grep -q '"wake_run_id":44' "$DIR_OTHER/recovery.json" || fail "other run has wrong identity"
check_v2 "$DIR_OTHER"
OTHER_FILES=$(cd "$DIR_OTHER" && cksum *)
rm -rf layout-a layout-b
"$WAKEBOX" -P 43 >/dev/null || fail "run-scoped recovery failed"
[ "$(cat layout-a/main.o)" = one ] && [ "$(cat layout-b/main.o)" = two ] || fail "run-scoped recovery produced wrong contents"
[ ! -e "$DIR_B" ] || fail "run-scoped recovery left $DIR_B"
[ ! -e "$DIR_C" ] || fail "run-scoped recovery left $DIR_C"
[ ! -e "$DIR_E" ] || fail "run-scoped recovery left $DIR_E"
[ -f "$DIR_OTHER/recovery.json" ] || fail "run-scoped recovery consumed another run"
[ "$(cd "$DIR_OTHER" && cksum *)" = "$OTHER_FILES" ] || fail "run-scoped recovery changed another run"

# Immediate materialization consumes the manifest returned by this execution only.
rm -rf layout-a layout-b
"$WAKEBOX" -m -o "$STATS_A" -p "$FIXTURES/input.json" >/dev/null || fail "immediate materialization failed"
DIR_M=$(field job_staging_root "$STATS_A")
[ -n "$DIR_M" ] && [ "$(field recovery_manifest "$STATS_A")" = "$DIR_M/recovery.json" ] || fail "immediate materialization returned the wrong manifest"
# The materializer removes the execution directory only for a v2 manifest.
[ ! -e "$DIR_M" ] || fail "immediate materialization left $DIR_M"
[ "$(cat layout-a/main.o)" = one ] && [ "$(cat layout-b/main.o)" = two ] || fail "immediate materialization produced wrong contents"
[ "$(cd "$DIR_OTHER" && cksum *)" = "$OTHER_FILES" ] || fail "immediate materialization changed another run"

echo PASS
