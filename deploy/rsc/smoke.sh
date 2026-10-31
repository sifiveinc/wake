#!/usr/bin/env bash
# Explicitly invoked cluster check; creates two pods and stops/restarts RSC.
set -euo pipefail
cd "$(dirname "$0")"
: "${RSC_NAMESPACE:?Set RSC_NAMESPACE to the namespace containing RSC}"
RSC_URL=${RSC_URL:-http://rsc:3002}
run_id=$(python3 -c 'import uuid; print(uuid.uuid4())')
k=(kubectl --namespace "$RSC_NAMESPACE")
for pod in rsc-smoke-push rsc-smoke-pull; do
    existing=$("${k[@]}" get pod "$pod" --ignore-not-found -o name)
    if [[ -n "$existing" ]]; then
        echo "Remove the existing $pod before running the smoke test" >&2
        exit 1
    fi
done
cleanup() {
    "${k[@]}" delete pod rsc-smoke-push rsc-smoke-pull --ignore-not-found --wait=false
}
trap cleanup EXIT
python3 render.py clients | "${k[@]}" create -f -
"${k[@]}" wait --for=condition=Ready pod/rsc-smoke-push pod/rsc-smoke-pull --timeout=180s
"${k[@]}" exec -i rsc-smoke-push -- python3 - push "$RSC_URL" "$run_id" < smoke.py
"${k[@]}" exec -i rsc-smoke-pull -- python3 - pull "$RSC_URL" "$run_id" < smoke.py
echo "Stopping RSC completely before recreating its pod with the same PVC"
"${k[@]}" scale deployment/rsc --replicas=0
"${k[@]}" wait --for=delete pod -l app.kubernetes.io/name=rsc --timeout=180s
"${k[@]}" scale deployment/rsc --replicas=1
"${k[@]}" rollout status deployment/rsc --timeout=180s
"${k[@]}" exec -i rsc-smoke-pull -- python3 - pull "$RSC_URL" "$run_id" < smoke.py
echo "PASS: RSC cache bytes and metadata survived pod replacement"
