#!/bin/sh
# Check Wake's recovery hint after cancellation for job-local staging directories.
# This exercises hint detection, not manifest parsing or recovery.
#
# Each case starts a long-running job, reads its run/job IDs from the Wakebox spec,
# creates synthetic staging paths, then cancels Wake. The "job" case expects a hint
# for a regular recovery.json in the matching job directory. Its contents are invalid
# on purpose: hint detection checks the path, not the JSON. The "unrelated" case
# expects no hint for another job, a nested private tree, an old-style private root,
# or a symlink where the matching manifest would be.
set -eu

WAKE="$(cd "${1:?expected the Wake binary directory}" && pwd)/wake"
FIXTURE="$(pwd)/test.wake"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/wake-cancellation-hint.XXXXXXXX")
pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill -TERM "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
    for _ in $(seq 1 100); do
        running=false
        for marker in "$WORK"/*/.fuse/*/.f.fuse-waked; do
            [ ! -e "$marker" ] || running=true
        done
        [ "$running" = false ] && break
        sleep 0.1
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

run_case() {
    case_name=$1
    mkdir "$WORK/$case_name"
    cd "$WORK/$case_name"
    cp "$FIXTURE" test.wake
    touch .wakeroot
    export CANCELED_READY="$WORK/$case_name.ready"
    "$WAKE" test >wake.out 2>wake.err &
    pid=$!
    # Wait for the payload to start before canceling, rather than relying on a fixed delay.
    for _ in $(seq 1 100); do
        [ -f "$CANCELED_READY" ] && break
        kill -0 "$pid" 2>/dev/null || fail "$case_name: Wake exited before the job started"
        sleep 0.1
    done
    [ -f "$CANCELED_READY" ] || fail "$case_name: job did not start"

    # Use the actual IDs assigned to this run when constructing the staging paths.
    set -- .build/spec-*.json
    [ "$#" = 1 ] && [ -f "$1" ] || fail "$case_name: expected one job spec"
    job_id=$(sed -n 's/.*"wake_job_id": *\([0-9]*\).*/\1/p' "$1")
    run_id=$(sed -n 's/.*"wake_run_id": *\([0-9]*\).*/\1/p' "$1")
    [ -n "$job_id" ] && [ -n "$run_id" ] || fail "$case_name: missing job identity"
    staging=.build/cas/staging
    uuid=0123456789abcdef0123456789abcdef

    case "$case_name" in
        unrelated)
            mkdir -p "$staging/job-$((job_id + 100000))-execution-$uuid" \
                "$staging/job-$job_id-execution-$uuid/workspace" "$staging/job-$job_id.1234.0"
            touch "$staging/job-$((job_id + 100000))-execution-$uuid/recovery.json" \
                "$staging/job-$job_id-execution-$uuid/workspace/recovery.json" \
                "$staging/job-$job_id.1234.0/recovery.json"
            ln -s "$FIXTURE" "$staging/job-$job_id-execution-$uuid/recovery.json"
            ;;
        job)
            mkdir -p "$staging/job-$job_id-execution-$uuid"
            # The hint checks the path, not the manifest contents.
            printf '%s\n' 'not a manifest' >"$staging/job-$job_id-execution-$uuid/recovery.json"
            ;;
    esac

    kill -TERM "$pid"
    wait "$pid" || :
    pid=
    if [ "$case_name" = unrelated ]; then
        if grep -q 'staged outputs remain to be recovered' wake.err; then
            fail "$case_name: unrelated directory triggered a hint"
        fi
    else
        grep -q "wakebox --materialize-previous $run_id" wake.err || fail "$case_name: missing hint"
    fi
}

run_case unrelated
run_case job
echo PASS
