#!/usr/bin/env bash
set -euo pipefail

log=${1:?usage: resolve-core-tester.sh <log-file>}
repo=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}
target_sha=${TARGET_SHA:?TARGET_SHA is required}

mkdir -p "$(dirname "$log")"
: > "$log"
exec > >(tee -a "$log") 2>&1

run_id=''
for attempt in $(seq 1 45); do
  run_id="$(gh api "repos/$repo/actions/workflows/windows-arm64-core.yml/runs?head_sha=$target_sha&per_page=20" --jq '.workflow_runs[0].id // empty')"
  if [[ -n "$run_id" ]]; then
    break
  fi
  echo "Exact-head Core run has not appeared yet ($attempt/45)."
  sleep 20
done
[[ -n "$run_id" ]] || { echo "ERROR: no Windows ARM64 Core run found for $target_sha"; exit 1; }

for attempt in $(seq 1 60); do
  status="$(gh api "repos/$repo/actions/runs/$run_id" --jq '.status')"
  conclusion="$(gh api "repos/$repo/actions/runs/$run_id" --jq '.conclusion // empty')"
  echo "Core run $run_id: status=$status conclusion=${conclusion:-pending}"
  if [[ "$status" == 'completed' ]]; then
    [[ "$conclusion" == 'success' ]] || { echo "ERROR: exact-head Core run concluded $conclusion"; exit 1; }
    break
  fi
  sleep 15
done

status="$(gh api "repos/$repo/actions/runs/$run_id" --jq '.status')"
conclusion="$(gh api "repos/$repo/actions/runs/$run_id" --jq '.conclusion // empty')"
[[ "$status" == 'completed' && "$conclusion" == 'success' ]] || {
  echo 'ERROR: exact-head Core run did not complete successfully in time.'
  exit 1
}

artifact_count="$(gh api "repos/$repo/actions/runs/$run_id/artifacts?per_page=100" --jq '[.artifacts[] | select(.name == "ipasim-ipa-tester" and .expired == false)] | length')"
[[ "$artifact_count" == '1' ]] || {
  echo "ERROR: expected one live ipasim-ipa-tester artifact, found $artifact_count"
  exit 1
}

echo "Resolved exact-head Core run: $run_id"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "run_id=$run_id" >> "$GITHUB_OUTPUT"
fi
