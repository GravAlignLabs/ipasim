#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
WORKSPACE="$REPO_ROOT/out/local-theos-preflight"
VENV="$WORKSPACE/venv"

missing=()
command -v python3 >/dev/null 2>&1 || missing+=(python3)
command -v git >/dev/null 2>&1 || missing+=(git)

if ((${#missing[@]})); then
  printf 'Missing required WSL/Ubuntu command(s): %s\n' "${missing[*]}" >&2
  echo "Install the local preflight prerequisites with:" >&2
  echo "  sudo apt update && sudo apt install -y python3 python3-venv git clang" >&2
  exit 2
fi

mkdir -p "$WORKSPACE"
if [[ ! -x "$VENV/bin/python" ]]; then
  echo "[local-preflight] creating isolated Python environment"
  if ! python3 -m venv "$VENV"; then
    echo "Could not create the virtual environment." >&2
    echo "Install python3-venv, then retry:" >&2
    echo "  sudo apt update && sudo apt install -y python3-venv" >&2
    exit 2
  fi
fi

PYTHON="$VENV/bin/python"
if ! "$PYTHON" -c 'import yaml; raise SystemExit(0 if yaml.__version__ == "6.0.2" else 1)' >/dev/null 2>&1; then
  echo "[local-preflight] installing pinned PyYAML==6.0.2"
  "$PYTHON" -m pip install --disable-pip-version-check PyYAML==6.0.2
fi

if (($# == 0)); then
  set -- full
fi

cd "$REPO_ROOT"
exec "$PYTHON" tools/compat_surface/local_theos_preflight.py "$@"
