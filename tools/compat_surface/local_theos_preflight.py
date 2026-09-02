#!/usr/bin/env python3
"""Developer-machine mirror of the pinned Theos SDK compatibility preflight."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_ROOT = REPO_ROOT / "tools" / "compat_surface"
BASELINE_PATH = TOOLS_ROOT / "local_theos_preflight_baseline.json"

THEOS_SDK_REPOSITORY = "https://github.com/theos/sdks.git"
THEOS_SDK_COMMIT = "0222fd5413cf4b9af096f37b4621afa2688572f7"
SDK_RELATIVE_PATH = Path("iPhoneOS16.5.sdk")
HEADER_SHARD_COUNT = 32
HEADER_TARGET = "arm64-apple-ios16.0"
HEADER_JOBS_PER_SHARD = 2
HEADER_PROGRESS_EVERY = 10
CLANG_TIMEOUT_SECONDS = 120
SHARD_WALL_TIMEOUT_SECONDS = 15 * 60
FULL_PREFLIGHT_WALL_TIMEOUT_SECONDS = 35 * 60
COMPILER_BATCH_SIZE = 256
HEADER_CACHE_INPUTS = (
    "header_surface.py",
    "sdk_header_context.py",
    "sdk_header_surface.py",
    "sdk_header_exhaustive.py",
)


class LocalPreflightError(RuntimeError):
    pass


def run(
    command: Sequence[str],
    *,
    capture: bool = False,
    check: bool = True,
    stdout=None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        cwd=REPO_ROOT,
        check=check,
        text=True if stdout is None else False,
        encoding="utf-8" if stdout is None else None,
        errors="replace" if stdout is None else None,
        stdout=subprocess.PIPE if capture else stdout,
        stderr=subprocess.PIPE if capture else (subprocess.STDOUT if stdout is not None else None),
    )


def git_output(*args: str) -> str | None:
    try:
        return run(["git", *args], capture=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def repo_head() -> str:
    return git_output("rev-parse", "HEAD") or "unknown"


def load_baseline() -> dict:
    try:
        value = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise LocalPreflightError(f"cannot read local preflight baseline: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise LocalPreflightError("unsupported local preflight baseline schema")
    return value


def workflow_blob_sha() -> str | None:
    return git_output("hash-object", ".github/workflows/theos-sdk-preflight.yml")


def print_banner(baseline: dict) -> None:
    theos = baseline.get("theos_preflight") or {}
    analyzer = baseline.get("compatibility_surface_analyzer") or {}
    print("=" * 72)
    print("ipaSim local Theos SDK preflight")
    print(
        f"CI parity baseline: Theos run #{theos.get('run_number', '?')} "
        f"(head {str(theos.get('head_sha', 'unknown'))[:12]})"
    )
    print(
        f"Fast analyzer baseline: run #{analyzer.get('run_number', '?')} "
        f"({analyzer.get('conclusion_at_sync', 'unknown')})"
    )
    print(f"Pinned SDK: theos/sdks@{THEOS_SDK_COMMIT} / {SDK_RELATIVE_PATH}")
    expected = baseline.get("ci_workflow_blob_sha")
    actual = workflow_blob_sha()
    if expected and actual and expected != actual:
        print("WARNING: CI WORKFLOW PARITY DRIFT")
        print("  .github/workflows/theos-sdk-preflight.yml changed after the")
        print("  checked-in local pipeline baseline. Review and resynchronize it.")
    print("=" * 72)


def clang_identity() -> str:
    if shutil.which("clang") is None:
        return "clang-unavailable"
    try:
        lines = run(["clang", "--version"], capture=True).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        return "clang-unavailable"
    return lines[0].strip() if lines else "clang-unknown"


def hash_file(hasher, path: Path, name: str) -> None:
    hasher.update(name.encode())
    hasher.update(b"\0")
    hasher.update(path.read_bytes())
    hasher.update(b"\0")


def header_cache_key() -> str:
    digest = hashlib.sha256()
    for name in HEADER_CACHE_INPUTS:
        hash_file(digest, TOOLS_ROOT / name, name)
    for value in (
        THEOS_SDK_COMMIT,
        SDK_RELATIVE_PATH.as_posix(),
        HEADER_TARGET,
        str(HEADER_SHARD_COUNT),
        str(CLANG_TIMEOUT_SECONDS),
        clang_identity(),
    ):
        digest.update(value.encode())
        digest.update(b"\0")
    return digest.hexdigest()[:20]


def pipeline_cache_key() -> str:
    digest = hashlib.sha256()
    for path in sorted(TOOLS_ROOT.glob("*.py")):
        if path.name.startswith("test_") or path.name == "local_theos_preflight.py":
            continue
        hash_file(digest, path, path.name)
    for path in sorted(TOOLS_ROOT.glob("*.json")):
        if path.name == BASELINE_PATH.name:
            continue
        hash_file(digest, path, path.name)
    digest.update(header_cache_key().encode())
    digest.update(str(COMPILER_BATCH_SIZE).encode())
    return digest.hexdigest()[:20]


def ensure_tools() -> None:
    missing = [name for name in ("git", "clang", "timeout") if shutil.which(name) is None]
    if missing:
        raise LocalPreflightError(
            "missing command(s): "
            + ", ".join(missing)
            + ". In Ubuntu/WSL run: sudo apt update && "
            "sudo apt install -y python3 python3-venv git clang"
        )


def ensure_sdk(workspace: Path) -> Path:
    checkout = workspace / "theos-sdks"
    if not (checkout / ".git").is_dir():
        checkout.parent.mkdir(parents=True, exist_ok=True)
        print(f"[local-preflight] creating sparse SDK checkout: {checkout}")
        run(
            [
                "git",
                "clone",
                "--filter=blob:none",
                "--no-checkout",
                "--depth",
                "1",
                THEOS_SDK_REPOSITORY,
                str(checkout),
            ]
        )
    run(["git", "-C", str(checkout), "remote", "set-url", "origin", THEOS_SDK_REPOSITORY])
    run(["git", "-C", str(checkout), "sparse-checkout", "init", "--no-cone"])
    run(
        [
            "git",
            "-C",
            str(checkout),
            "sparse-checkout",
            "set",
            "--no-cone",
            SDK_RELATIVE_PATH.as_posix(),
        ]
    )
    print(f"[local-preflight] ensuring SDK commit {THEOS_SDK_COMMIT[:12]}")
    run(["git", "-C", str(checkout), "fetch", "--depth", "1", "origin", THEOS_SDK_COMMIT])
    run(["git", "-C", str(checkout), "checkout", "--detach", "--force", "FETCH_HEAD"])
    actual = run(["git", "-C", str(checkout), "rev-parse", "HEAD"], capture=True).stdout.strip()
    if actual != THEOS_SDK_COMMIT:
        raise LocalPreflightError(f"SDK checkout drift: expected {THEOS_SDK_COMMIT}, got {actual}")
    sdk_root = (checkout / SDK_RELATIVE_PATH).resolve()
    if not sdk_root.is_dir():
        raise LocalPreflightError(f"SDK was not materialized: {sdk_root}")
    return sdk_root


def run_tests() -> None:
    print("[local-preflight] running compatibility surface tests")
    run(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "tools/compat_surface",
            "-p",
            "test_*.py",
            "-v",
        ]
    )


def read_manifest(path: Path) -> dict | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def valid_cached_shard(path: Path, index: int) -> bool:
    manifest = read_manifest(path)
    coverage = manifest.get("coverage") if manifest else None
    return bool(
        manifest
        and manifest.get("schema_version") == 1
        and manifest.get("kind") == "header-signature-surface"
        and manifest.get("target") == HEADER_TARGET
        and isinstance(coverage, dict)
        and coverage.get("schema_version") == 1
        and coverage.get("strategy") == "sorted-round-robin"
        and coverage.get("shard_count") == HEADER_SHARD_COUNT
        and coverage.get("shard_index") == index
        and isinstance(coverage.get("sdk_header_count"), int)
        and coverage.get("sdk_header_count") > 0
        and isinstance(coverage.get("headers"), list)
    )


def tail(path: Path, count: int = 100) -> str:
    try:
        return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-count:])
    except OSError:
        return ""


def timed_command(command: Sequence[str], *, seconds: int, log: Path) -> tuple[int, float]:
    log.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    with log.open("wb") as stream:
        result = run(
            [
                "timeout",
                "--signal=TERM",
                "--kill-after=30s",
                f"{seconds}s",
                *command,
            ],
            check=False,
            stdout=stream,
        )
    return result.returncode, time.monotonic() - started


def run_one_shard(
    *,
    sdk_root: Path,
    shard_dir: Path,
    index: int,
    jobs: int,
    clang_timeout: int,
    wall_timeout: int,
    force: bool,
) -> tuple[int, bool, float, Path]:
    manifest = shard_dir / f"header-shard-{index}.json"
    log = shard_dir / f"header-shard-{index}.log"
    status = shard_dir / f"header-shard-{index}.status"
    if not force and valid_cached_shard(manifest, index):
        return index, True, 0.0, log
    for path in (manifest, log, status):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
    command = [
        sys.executable,
        str(TOOLS_ROOT / "sdk_header_exhaustive.py"),
        "--sdk-root",
        str(sdk_root),
        "--shard-count",
        str(HEADER_SHARD_COUNT),
        "--shard-index",
        str(index),
        "--jobs",
        str(jobs),
        "--progress-every",
        str(HEADER_PROGRESS_EVERY),
        "--timeout",
        str(clang_timeout),
        "--output",
        str(manifest),
    ]
    code, elapsed = timed_command(command, seconds=wall_timeout, log=log)
    status.write_text(f"{code}\n", encoding="utf-8")
    return index, code == 0 and valid_cached_shard(manifest, index), elapsed, log


def selected_shards(values: Sequence[int]) -> list[int]:
    selected = sorted(set(values)) if values else list(range(HEADER_SHARD_COUNT))
    bad = [item for item in selected if item < 0 or item >= HEADER_SHARD_COUNT]
    if bad:
        raise LocalPreflightError(f"invalid shard index: {bad}")
    return selected


def run_shards(
    *,
    sdk_root: Path,
    workspace: Path,
    selected: Sequence[int],
    parallel: int,
    jobs: int,
    clang_timeout: int,
    wall_timeout: int,
    force: bool,
) -> Path:
    if parallel <= 0 or jobs <= 0:
        raise LocalPreflightError("worker counts must be positive")
    shard_dir = workspace / "cache" / header_cache_key() / "header-shards"
    shard_dir.mkdir(parents=True, exist_ok=True)
    wanted = selected_shards(selected)
    reusable = [
        index
        for index in wanted
        if not force and valid_cached_shard(shard_dir / f"header-shard-{index}.json", index)
    ]
    pending = [index for index in wanted if index not in reusable]
    if reusable:
        print("[local-preflight] reusing shards: " + ", ".join(map(str, reusable)))
    if not pending:
        return shard_dir
    print(
        f"[local-preflight] running {len(pending)} shard(s), "
        f"parallel={min(parallel, len(pending))}, clang-workers/shard={jobs}"
    )
    failures: list[tuple[int, Path]] = []
    with ThreadPoolExecutor(max_workers=min(parallel, len(pending))) as pool:
        futures = {
            pool.submit(
                run_one_shard,
                sdk_root=sdk_root,
                shard_dir=shard_dir,
                index=index,
                jobs=jobs,
                clang_timeout=clang_timeout,
                wall_timeout=wall_timeout,
                force=force,
            ): index
            for index in pending
        }
        for future in as_completed(futures):
            index, ok, elapsed, log = future.result()
            print(f"[local-preflight] shard {index:02d} {'OK' if ok else 'FAILED'} ({elapsed:.1f}s)")
            if not ok:
                failures.append((index, log))
    if failures:
        failures.sort()
        index, log = failures[0]
        detail = tail(log, 120)
        raise LocalPreflightError(
            f"{len(failures)} shard(s) failed; first={index}"
            + (f"\n--- shard {index} tail ---\n{detail}" if detail else "")
        )
    return shard_dir


def all_manifests(shard_dir: Path) -> list[Path]:
    manifests = []
    missing = []
    for index in range(HEADER_SHARD_COUNT):
        path = shard_dir / f"header-shard-{index}.json"
        if valid_cached_shard(path, index):
            manifests.append(path)
        else:
            missing.append(index)
    if missing:
        raise LocalPreflightError(f"downstream requires all current shards; missing={missing}")
    return manifests


def run_downstream(
    *,
    sdk_root: Path,
    workspace: Path,
    shard_dir: Path,
    clang_timeout: int,
    wall_timeout: int,
    force: bool,
) -> Path:
    manifests = all_manifests(shard_dir)
    output = workspace / "output" / pipeline_cache_key()
    bundle = output / "bundle"
    log = output / "sdk-preflight.log"
    if (
        not force
        and (bundle / "compatibility-plan.json").is_file()
        and (bundle / "header-signatures.json").is_file()
    ):
        print(f"[local-preflight] reusing completed bundle: {bundle}")
        return bundle
    output.mkdir(parents=True, exist_ok=True)
    if bundle.exists():
        shutil.rmtree(bundle)
    command = [
        sys.executable,
        str(TOOLS_ROOT / "sdk_compatibility.py"),
        "--sdk-root",
        str(sdk_root),
        "--output-dir",
        str(bundle),
        "--compiler-batch-size",
        str(COMPILER_BATCH_SIZE),
        "--timeout",
        str(clang_timeout),
    ]
    for manifest in manifests:
        command += ["--header-manifest", str(manifest)]
    print("[local-preflight] running shard merge + complete SDK compatibility pipeline")
    code, elapsed = timed_command(command, seconds=wall_timeout, log=log)
    (output / "run.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "repo_head": repo_head(),
                "header_cache_key": header_cache_key(),
                "pipeline_cache_key": pipeline_cache_key(),
                "theos_sdk_commit": THEOS_SDK_COMMIT,
                "exit_code": code,
                "elapsed_seconds": round(elapsed, 3),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    if code != 0:
        detail = tail(log, 180)
        raise LocalPreflightError(
            f"complete SDK preflight failed with exit code {code}"
            + (f"\n--- diagnostic tail ---\n{detail}" if detail else "")
        )
    print(f"[local-preflight] complete SDK preflight OK ({elapsed:.1f}s)")
    return bundle


def write_latest(workspace: Path, baseline: dict, command: str, sdk_root, bundle) -> None:
    workspace.mkdir(parents=True, exist_ok=True)
    (workspace / "latest.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "updated_epoch": time.time(),
                "command": command,
                "repo_head": repo_head(),
                "header_cache_key": header_cache_key(),
                "pipeline_cache_key": pipeline_cache_key(),
                "theos_sdk_commit": THEOS_SDK_COMMIT,
                "sdk_root": str(sdk_root) if sdk_root else None,
                "bundle": str(bundle) if bundle else None,
                "ci_parity_baseline": baseline,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def print_status(workspace: Path, baseline: dict) -> None:
    shard_dir = workspace / "cache" / header_cache_key() / "header-shards"
    valid = [
        index
        for index in range(HEADER_SHARD_COUNT)
        if valid_cached_shard(shard_dir / f"header-shard-{index}.json", index)
    ]
    expected = baseline.get("ci_workflow_blob_sha")
    actual = workflow_blob_sha()
    print(f"Repo head: {repo_head()}")
    print(f"Clang: {clang_identity()}")
    print(f"Header cache key: {header_cache_key()}")
    print(f"Pipeline cache key: {pipeline_cache_key()}")
    print(f"Valid cached shards: {len(valid)}/{HEADER_SHARD_COUNT}")
    print(f"Workspace: {workspace}")
    if expected and actual:
        print(f"CI workflow parity: {'MATCH' if expected == actual else 'DRIFT'} ({actual})")


def clean(workspace: Path, all_data: bool) -> None:
    if all_data:
        if workspace.exists():
            shutil.rmtree(workspace)
            print(f"[local-preflight] removed {workspace}")
        return
    for path in (
        workspace / "cache" / header_cache_key(),
        workspace / "output" / pipeline_cache_key(),
    ):
        if path.exists():
            shutil.rmtree(path)
            print(f"[local-preflight] removed {path}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--workspace",
        type=Path,
        default=REPO_ROOT / "out" / "local-theos-preflight",
        help="SDK/cache/output directory (default: out/local-theos-preflight)",
    )
    commands = result.add_subparsers(dest="command", required=True)

    full = commands.add_parser("full")
    full.add_argument("--skip-tests", action="store_true")
    full.add_argument("--force-shards", action="store_true")
    full.add_argument("--force-downstream", action="store_true")
    full.add_argument("--parallel-shards", type=int, default=4)
    full.add_argument("--jobs-per-shard", type=int, default=HEADER_JOBS_PER_SHARD)
    full.add_argument("--clang-timeout", type=int, default=CLANG_TIMEOUT_SECONDS)
    full.add_argument("--shard-wall-timeout", type=int, default=SHARD_WALL_TIMEOUT_SECONDS)
    full.add_argument("--full-wall-timeout", type=int, default=FULL_PREFLIGHT_WALL_TIMEOUT_SECONDS)

    commands.add_parser("prepare")
    commands.add_parser("tests")
    commands.add_parser("status")

    shards = commands.add_parser("shards")
    shards.add_argument("--shard", type=int, action="append", default=[])
    shards.add_argument("--force", action="store_true")
    shards.add_argument("--parallel-shards", type=int, default=4)
    shards.add_argument("--jobs-per-shard", type=int, default=HEADER_JOBS_PER_SHARD)
    shards.add_argument("--clang-timeout", type=int, default=CLANG_TIMEOUT_SECONDS)
    shards.add_argument("--shard-wall-timeout", type=int, default=SHARD_WALL_TIMEOUT_SECONDS)

    downstream = commands.add_parser("downstream")
    downstream.add_argument("--force", action="store_true")
    downstream.add_argument("--clang-timeout", type=int, default=CLANG_TIMEOUT_SECONDS)
    downstream.add_argument("--full-wall-timeout", type=int, default=FULL_PREFLIGHT_WALL_TIMEOUT_SECONDS)

    cleaner = commands.add_parser("clean")
    cleaner.add_argument("--all", action="store_true")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    workspace = args.workspace.resolve()
    baseline = load_baseline()
    print_banner(baseline)
    sdk_root = None
    bundle = None
    try:
        if args.command == "status":
            print_status(workspace, baseline)
        elif args.command == "clean":
            clean(workspace, args.all)
        elif args.command == "tests":
            ensure_tools()
            run_tests()
        elif args.command == "prepare":
            ensure_tools()
            sdk_root = ensure_sdk(workspace)
            print(f"[local-preflight] SDK ready: {sdk_root}")
        elif args.command == "shards":
            ensure_tools()
            sdk_root = ensure_sdk(workspace)
            run_shards(
                sdk_root=sdk_root,
                workspace=workspace,
                selected=args.shard,
                parallel=args.parallel_shards,
                jobs=args.jobs_per_shard,
                clang_timeout=args.clang_timeout,
                wall_timeout=args.shard_wall_timeout,
                force=args.force,
            )
        elif args.command == "downstream":
            ensure_tools()
            sdk_root = ensure_sdk(workspace)
            shard_dir = workspace / "cache" / header_cache_key() / "header-shards"
            bundle = run_downstream(
                sdk_root=sdk_root,
                workspace=workspace,
                shard_dir=shard_dir,
                clang_timeout=args.clang_timeout,
                wall_timeout=args.full_wall_timeout,
                force=args.force,
            )
        elif args.command == "full":
            ensure_tools()
            sdk_root = ensure_sdk(workspace)
            if not args.skip_tests:
                run_tests()
            shard_dir = run_shards(
                sdk_root=sdk_root,
                workspace=workspace,
                selected=(),
                parallel=args.parallel_shards,
                jobs=args.jobs_per_shard,
                clang_timeout=args.clang_timeout,
                wall_timeout=args.shard_wall_timeout,
                force=args.force_shards,
            )
            bundle = run_downstream(
                sdk_root=sdk_root,
                workspace=workspace,
                shard_dir=shard_dir,
                clang_timeout=args.clang_timeout,
                wall_timeout=args.full_wall_timeout,
                force=args.force_downstream,
            )
        else:
            raise LocalPreflightError(f"unknown command: {args.command}")
        write_latest(workspace, baseline, args.command, sdk_root, bundle)
        if bundle:
            print(f"[local-preflight] bundle: {bundle}")
        return 0
    except (LocalPreflightError, OSError, subprocess.CalledProcessError) as exc:
        print(f"[local-preflight] ERROR: {exc}", file=sys.stderr)
        try:
            write_latest(workspace, baseline, args.command, sdk_root, bundle)
        except OSError:
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
