#!/usr/bin/env bash
set -euo pipefail

output=${1:?usage: build-dwarfs-fixture.sh <output-image> <log-file>}
log=${2:?usage: build-dwarfs-fixture.sh <output-image> <log-file>}
version=${DWARFS_VERSION:-0.15.7}
sha=${DWARFS_UNIVERSAL_SHA256:-baa03026e7d2c195fdb78bf261cd7d20f620b20685f8a3e26162ba9d652b0d78}

mkdir -p "$(dirname "$output")" "$(dirname "$log")"
: > "$log"
exec > >(tee -a "$log") 2>&1

tool="${RUNNER_TEMP:-/tmp}/dwarfs-universal-${version}-Linux-x86_64"
root="${RUNNER_TEMP:-/tmp}/runtime-root-dwarfs-fixture"
list="${RUNNER_TEMP:-/tmp}/runtime-root-dwarfs-fixture-list.txt"

curl --fail --location --retry 3 \
  "https://github.com/mhx/dwarfs/releases/download/v${version}/dwarfs-universal-${version}-Linux-x86_64" \
  --output "$tool"
echo "${sha}  ${tool}" | sha256sum --check --strict
chmod +x "$tool"

rm -rf "$root"
mkdir -p "$root/System/Library/Frameworks/UIKit.framework/Versions/A:"
python3 - "$root/System/Library/Frameworks/UIKit.framework/Versions/A:/UIKit" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(bytes([0x49, 0x50, 0x41, 0x53, 0x69, 0x6d, 0x00, 0xff, 0x2f, 0x3a, 0x7f]))
PY

rm -f "$output"
"$tool" --tool=mkdwarfs -i "$root" -o "$output" --log-level=warn
"$tool" --tool=dwarfsck "$output" -l | tee "$list"
grep -F 'System/Library/Frameworks/UIKit.framework/Versions/A:/UIKit' "$list"
[[ -s "$output" ]] || { echo "ERROR: fixture DwarFS image was not created: $output"; exit 1; }

echo "Fixture DwarFS image: $output"
