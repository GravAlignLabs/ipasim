#!/usr/bin/env bash
set -euo pipefail

image=${1:?usage: build-runtime-dwarfs.sh <image> <log-file>}
log=${2:?usage: build-runtime-dwarfs.sh <image> <log-file>}
version=${DWARFS_VERSION:-0.15.7}
xcode=${RUNTIME_XCODE_PATH:-/Applications/Xcode_16.4.app}
ios_version=${RUNTIME_IOS_VERSION:-18.5}
ios_build=${RUNTIME_IOS_BUILD:-22F77}

mkdir -p "$(dirname "$image")" "$(dirname "$log")"
: > "$log"
exec > >(tee -a "$log") 2>&1

[[ -d "$xcode" ]] || { echo "ERROR: pinned Xcode is not installed: $xcode"; exit 1; }
sudo xcode-select -s "$xcode/Contents/Developer"
xcodebuild -version

runtime_json="${RUNNER_TEMP:-/tmp}/simctl-runtimes.json"
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
echo "Pinned RuntimeRoot: $runtime_root"

brew install dwarfs
installed_version="$(brew list --versions dwarfs | awk 'NR == 1 {print $2}')"
if [[ "$installed_version" != "$version" ]]; then
  echo "ERROR: expected Homebrew DwarFS $version but installed ${installed_version:-<none>}"
  exit 1
fi
prefix="$(brew --prefix dwarfs)"
mkdwarfs="$prefix/bin/mkdwarfs"
dwarfsck="$prefix/bin/dwarfsck"
for tool in "$mkdwarfs" "$dwarfsck"; do
  [[ -x "$tool" ]] || { echo "ERROR: expected DwarFS tool is missing: $tool"; exit 1; }
  if ! "$tool" -h >/dev/null 2>&1; then
    echo "ERROR: DwarFS tool did not accept its documented help option: $tool"
    exit 1
  fi
done
echo "Pinned DwarFS writer/checker: $installed_version ($prefix)"

build_seconds=0
cache_reused=false
if [[ -s "$image" ]]; then
  cache_reused=true
  echo "Using cached complete RuntimeRoot DwarFS image: $image"
else
  rm -f "$image"
  echo "Building one DwarFS image from the complete iOS $ios_version ($ios_build) RuntimeRoot."
  echo 'No RuntimeRoot paths are excluded, renamed, sanitized, mounted, or pre-extracted.'
  started="$(date +%s)"
  sudo "$mkdwarfs" -i "$runtime_root" -o "$image" --log-level=info
  finished="$(date +%s)"
  build_seconds=$((finished - started))
  sudo chown "$(id -u):$(id -g)" "$image"
  chmod 600 "$image"
  [[ -s "$image" ]] || { echo "ERROR: RuntimeRoot.dwarfs was not created: $image"; exit 1; }
  echo "DwarFS image build seconds: $build_seconds"
fi

echo 'Running DwarFS embedded block integrity validation.'
"$dwarfsck" "$image" --check-integrity
sha="$(shasum -a 256 "$image" | awk '{print tolower($1)}')"
size="$(stat -f '%z' "$image")"
[[ "$sha" =~ ^[0-9a-f]{64}$ ]] || { echo "ERROR: invalid image SHA-256: $sha"; exit 1; }
[[ "$size" =~ ^[0-9]+$ ]] || { echo "ERROR: invalid image size: $size"; exit 1; }

echo "RuntimeRoot.dwarfs SHA-256: $sha"
echo "RuntimeRoot.dwarfs bytes: $size"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  {
    echo "image_sha256=$sha"
    echo "image_size=$size"
    echo "build_seconds=$build_seconds"
    echo "cache_reused=$cache_reused"
  } >> "$GITHUB_OUTPUT"
fi
