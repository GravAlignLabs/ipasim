#!/usr/bin/env bash
set -euo pipefail

archive=${1:?usage: build-runtime-zstd.sh <archive> <log-file>}
log=${2:?usage: build-runtime-zstd.sh <archive> <log-file>}
xcode=${RUNTIME_XCODE_PATH:-/Applications/Xcode_16.4.app}
ios_version=${RUNTIME_IOS_VERSION:-18.5}
ios_build=${RUNTIME_IOS_BUILD:-22F77}

mkdir -p "$(dirname "$archive")" "$(dirname "$log")"
: > "$log"
exec > >(tee -a "$log") 2>&1

emit_archive_identity() {
  local sha size
  sha="$(shasum -a 256 "$archive" | awk '{print tolower($1)}')"
  size="$(stat -f '%z' "$archive")"
  [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || { echo "ERROR: invalid RuntimeRoot.tar.zst SHA-256: $sha"; exit 1; }
  [[ "$size" =~ ^[0-9]+$ ]] || { echo "ERROR: invalid RuntimeRoot.tar.zst size: $size"; exit 1; }

  echo "RuntimeRoot.tar.zst SHA-256: $sha"
  echo "RuntimeRoot.tar.zst bytes: $size"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    {
      echo "archive_sha256=$sha"
      echo "archive_size=$size"
      echo "build_seconds=${build_seconds:-0}"
      echo "cache_reused=${cache_reused:-false}"
    } >> "$GITHUB_OUTPUT"
  fi
}

build_seconds=0
cache_reused=false

# This is the same complete pinned iOS 18.5 (22F77) directory baseline
# contract used by trusted-github-runtime-acceptance.yml: sudo tar piped
# directly to zstd -1 -T0. No RuntimeRoot path is excluded or rewritten.
if [[ -s "$archive" ]]; then
  cache_reused=true
  echo "Using cached trusted RuntimeRoot.tar.zst baseline: $archive"
  emit_archive_identity
  exit 0
fi

[[ -d "$xcode" ]] || { echo "ERROR: pinned Xcode is not installed: $xcode"; exit 1; }
sudo xcode-select -s "$xcode/Contents/Developer"
xcodebuild -version

iphoneos_version="$(xcrun --sdk iphoneos --show-sdk-version)"
iphonesimulator_version="$(xcrun --sdk iphonesimulator --show-sdk-version)"
[[ "$iphoneos_version" == "$ios_version" ]] || {
  echo "ERROR: pinned iphoneos SDK changed: expected $ios_version, got $iphoneos_version"
  exit 1
}
[[ "$iphonesimulator_version" == "$ios_version" ]] || {
  echo "ERROR: pinned iphonesimulator SDK changed: expected $ios_version, got $iphonesimulator_version"
  exit 1
}

runtime_json="${RUNNER_TEMP:-/tmp}/simctl-runtimes-zstd.json"
xcrun simctl list runtimes --json > "$runtime_json"
runtime_bundle="$(python3 - "$runtime_json" "$ios_version" "$ios_build" <<'PY'
import json
import sys

path, target_version, target_build = sys.argv[1:]
with open(path, 'r', encoding='utf-8') as handle:
    data = json.load(handle)

for runtime in data.get('runtimes', []):
    if runtime.get('version') != target_version:
        continue
    if runtime.get('buildversion') != target_build:
        continue
    if runtime.get('isAvailable') is False:
        continue
    bundle = runtime.get('bundlePath') or runtime.get('dataPath')
    if bundle:
        print(bundle)
        raise SystemExit(0)

print(f'pinned iOS runtime {target_version} ({target_build}) not found', file=sys.stderr)
raise SystemExit(1)
PY
)"

runtime_root="$runtime_bundle/Contents/Resources/RuntimeRoot"
[[ -d "$runtime_root/System" && -d "$runtime_root/usr" ]] || {
  echo "ERROR: pinned RuntimeRoot does not expose System and usr: $runtime_root"
  exit 1
}

zstd_bin="$(command -v zstd || true)"
[[ -n "$zstd_bin" ]] || {
  echo 'ERROR: zstd is not available on this GitHub-hosted macOS runner.'
  exit 1
}

echo "Pinned RuntimeRoot: $runtime_root"
echo "Creating trusted RuntimeRoot.tar.zst from complete iOS $ios_version ($ios_build) RuntimeRoot."
echo 'Using sudo tar | zstd -1 -T0. No RuntimeRoot paths are excluded.'

rm -f "$archive"
runtime_parent="$(dirname "$runtime_root")"
runtime_leaf="$(basename "$runtime_root")"
started="$(date +%s)"
(
  cd "$runtime_parent"
  sudo tar -cf - "$runtime_leaf" | "$zstd_bin" -1 -T0 -q -o "$archive"
)
finished="$(date +%s)"
build_seconds=$((finished - started))

[[ -s "$archive" ]] || {
  echo "ERROR: RuntimeRoot.tar.zst was not created: $archive"
  exit 1
}
chmod 600 "$archive"
echo "RuntimeRoot zstd build seconds: $build_seconds"
emit_archive_identity
