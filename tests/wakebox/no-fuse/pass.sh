#!/bin/sh
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
[ "$(uname -s)" = Linux ] || exit 0

WAKEBOX="$(cd "${1:?expected the Wake binary directory}" && pwd)/wakebox"
INPUT="$(pwd)/input.json"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/wakebox-no-fuse.XXXXXXXX")
pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill -KILL "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
fail() {
    echo "FAIL: $*" >&2
    [ ! -s err ] || cat err >&2
    exit 1
}

cd "$WORK"
cp "$INPUT" input.json
mkdir private

"$WAKEBOX" --no-fuse-daemon -p input.json -o result.json >out 2>err || fail "execution failed"
[ -f private/executed ] || fail "payload did not run in the private tree"
[ ! -e executed ] || fail "payload ran outside the private tree"
jq -e '.usage.status == 0 and .outputs == [] and .staging_files == {} and
       (has("recovery_manifest") | not) and (has("job_staging_root") | not)' result.json >/dev/null || fail "invalid no-FUSE result"
[ ! -e .build/cas/staging ] || fail "no-FUSE execution created staging"
[ ! -e .fuse ] || fail "no-FUSE execution contacted the FUSE daemon"

# Wait for the payload rather than racing the signal handler's installation.
jq '.command = ["/bin/sh", "-c", ": > ready; exec sleep 60"]' input.json >cancel.json
"$WAKEBOX" --no-fuse-daemon -I -p cancel.json -o canceled.json >out 2>err &
pid=$!
for i in $(seq 1 100); do
    [ -f private/ready ] && break
    kill -0 "$pid" 2>/dev/null || fail "cancellation payload did not start"
    sleep 0.1
done
[ -f private/ready ] || fail "timed out waiting for cancellation payload"
kill -TERM "$pid"
for i in $(seq 1 100); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$pid" 2>/dev/null && fail "cancellation did not finish"
status=0
wait "$pid" || status=$?
pid=
[ "$status" -ne 0 ] || fail "-I hid cancellation"
jq -e '.["canceled-signal"] == 15 and .outputs == [] and .staging_files == {} and
       (has("recovery_manifest") | not) and (has("job_staging_root") | not)' canceled.json >/dev/null || fail "invalid canceled result"
if grep -qi materializ err out; then fail "cancellation attempted materialization"; fi
[ ! -e .build/cas/staging ] || fail "cancellation created staging"
[ ! -e .fuse ] || fail "cancellation contacted the FUSE daemon"
