#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <7z1|gzip1|zstd1> <RuntimeRoot> <output-dir>" >&2
  exit 2
fi

candidate="$1"
runtime_root="$2"
out_dir="$3"

if [[ ! -d "$runtime_root/System" || ! -d "$runtime_root/usr" ]]; then
  echo "ERROR: RuntimeRoot must expose System and usr: $runtime_root" >&2
  exit 1
fi

mkdir -p "$out_dir"
runtime_parent="$(dirname "$runtime_root")"
runtime_leaf="$(basename "$runtime_root")"

logical_kib="$(sudo du -sk "$runtime_root" | awk '{print $1}')"
file_count="$(sudo find "$runtime_root" -type f -print | wc -l | tr -d ' ')"
start_epoch="$(date +%s)"

case "$candidate" in
  7z1)
    seven_zip="$(command -v 7zz || command -v 7z || true)"
    if [[ -z "$seven_zip" ]]; then
      echo 'ERROR: 7-Zip is not installed on this runner.' >&2
      exit 1
    fi
    archive="$out_dir/RuntimeRoot.7z"
    (
      cd "$runtime_parent"
      sudo "$seven_zip" a -t7z "$archive" "$runtime_leaf" -mx=1
    )
    ;;
  gzip1)
    archive="$out_dir/RuntimeRoot.tar.gz"
    (
      cd "$runtime_parent"
      sudo tar -cf - "$runtime_leaf" | gzip -1 > "$archive"
    )
    ;;
  zstd1)
    zstd_bin="$(command -v zstd || true)"
    if [[ -z "$zstd_bin" ]]; then
      echo 'ERROR: zstd is not installed on this runner.' >&2
      exit 1
    fi
    archive="$out_dir/RuntimeRoot.tar.zst"
    (
      cd "$runtime_parent"
      sudo tar -cf - "$runtime_leaf" | "$zstd_bin" -1 -T0 -q -o "$archive"
    )
    ;;
  *)
    echo "ERROR: unknown candidate: $candidate" >&2
    exit 2
    ;;
esac

end_epoch="$(date +%s)"
elapsed_seconds="$((end_epoch - start_epoch))"

if [[ ! -s "$archive" ]]; then
  echo "ERROR: candidate archive was not created: $archive" >&2
  exit 1
fi

# sudo-created 7z archives can be root-owned. Make cleanup deterministic.
sudo chown "$(id -u):$(id -g)" "$archive" 2>/dev/null || true
archive_bytes="$(stat -f '%z' "$archive")"

printf 'candidate=%s\n' "$candidate"
printf 'logical_kib=%s\n' "$logical_kib"
printf 'file_count=%s\n' "$file_count"
printf 'elapsed_seconds=%s\n' "$elapsed_seconds"
printf 'archive_bytes=%s\n' "$archive_bytes"
printf 'archive_path=%s\n' "$archive"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  {
    echo "candidate=$candidate"
    echo "logical_kib=$logical_kib"
    echo "file_count=$file_count"
    echo "elapsed_seconds=$elapsed_seconds"
    echo "archive_bytes=$archive_bytes"
  } >> "$GITHUB_OUTPUT"
fi

rm -f "$archive"
