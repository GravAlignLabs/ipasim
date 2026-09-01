#!/usr/bin/env python3
"""Run ipaSim's complete SDK-wide compatibility pipeline from one Apple SDK root.

This is orchestration only. Existing analyzers remain authoritative for TAPI,
header signatures, AAPCS64/Win64 ABI lowering, bridge plans, runtime adapters,
compatibility planning, and explicit semantic-provider approval.

Header analysis may be performed in deterministic shards and supplied back to
this command. Sharded input is accepted only after proving that the shard set
covers the exact requested SDK header inventory once, with no gaps, overlaps,
or target drift. The downstream compatibility pipeline is otherwise unchanged.

The command intentionally fails closed. A TAPI/header/compiler/semantic-provider
error aborts the run before the output bundle is materialized.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Sequence

import abi_surface
import bridge_plan
import bulk_compatibility
import compat_planner
import compiler_batching
import generate_semantic_routes
import header_surface
import runtime_adapter_table
import sdk_catalog
import sdk_header_exhaustive as sdk_header_surface
import tbd_surface
import win64_abi_surface


DEFAULT_SEMANTIC_PROVIDERS = Path(__file__).with_name("semantic_providers.json")


class SdkCompatibilityError(ValueError):
    """Raised when a complete SDK compatibility run cannot finish safely."""


def _validate_sdk_root(path: Path) -> Path:
    root = path.resolve()
    if not root.is_dir():
        raise SdkCompatibilityError(f"SDK root does not exist: {path.name}")
    return root


def _collect_headers(
    sdk_root: Path,
    relative_headers: Sequence[str] = (),
) -> list[tuple[Path, str]]:
    """Return the canonical physical SDK header inventory.

    The shard scanner canonicalizes each SDK-relative header through ``resolve``
    and then keeps only the first deterministic display path for a physical
    file. The merge stage must reconstruct that exact same inventory. Apple SDKs
    contain header aliases/symlinks, so counting every directory entry here can
    otherwise make a valid shard set appear to have the wrong SDK header count.
    """
    root = sdk_root.resolve()
    if relative_headers:
        relatives = [Path(item) for item in relative_headers]
    else:
        # Match header_surface._resolve_inputs exactly. Path ordering compares
        # path components, which intentionally differs from sorting as_posix()
        # when a file and directory share a prefix (for example pthread.h and
        # pthread/introspection.h). The first path wins when aliases resolve to
        # the same physical header, so the ordering itself is part of coverage.
        relatives = sorted(
            path.relative_to(root)
            for path in root.rglob("*.h")
            if path.is_file()
        )

    unique: dict[str, tuple[Path, str]] = {}
    for relative in relatives:
        if relative.is_absolute() or ".." in relative.parts:
            raise SdkCompatibilityError(
                f"relative header must stay inside SDK root: {relative}"
            )
        path = (root / relative).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise SdkCompatibilityError(
                f"relative header escapes SDK root: {relative}"
            ) from exc
        if not path.is_file():
            raise SdkCompatibilityError(
                f"SDK header does not exist: {relative.as_posix()}"
            )
        unique.setdefault(str(path), (path, relative.as_posix()))

    if not unique:
        raise SdkCompatibilityError("no SDK headers were found")
    return sorted(unique.values(), key=lambda item: item[1])


def _build_tapi_manifest(
    sdk_root: Path,
    targets: Sequence[str] | None,
) -> dict:
    requested = tbd_surface._normalize_requested_targets(targets)
    interfaces = []
    for path, display in tbd_surface._collect_inputs([], str(sdk_root)):
        interfaces.extend(
            tbd_surface.parse_tbd_file(
                path,
                display,
                requested,
            )
        )
    return tbd_surface.build_sdk_manifest(interfaces)


def _build_header_manifest(
    sdk_root: Path,
    *,
    relative_headers: Sequence[str],
    jobs: int,
    clang: str,
    target: str,
    clang_args: Sequence[str],
    timeout_seconds: int,
) -> dict:
    inputs = _collect_headers(sdk_root, relative_headers)
    return sdk_header_surface.build_parallel_manifest(
        inputs,
        jobs=jobs,
        clang=clang,
        target=target,
        sdk_root=sdk_root,
        extra_args=clang_args,
        timeout_seconds=timeout_seconds,
    )


def _load_header_shards(paths: Sequence[Path]) -> list[dict]:
    manifests = []
    for path in paths:
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError as exc:
            raise SdkCompatibilityError(
                f"header shard manifest does not exist: {path.name}"
            ) from exc
        except json.JSONDecodeError as exc:
            raise SdkCompatibilityError(
                f"header shard manifest is not valid JSON: {path.name}: {exc}"
            ) from exc
        if not isinstance(manifest, dict):
            raise SdkCompatibilityError(
                f"header shard manifest must contain an object: {path.name}"
            )
        manifests.append(manifest)
    if not manifests:
        raise SdkCompatibilityError("no header shard manifests were supplied")
    return manifests


def _merge_header_shards(
    sdk_root: Path,
    *,
    relative_headers: Sequence[str],
    header_manifests: Sequence[Path],
    target: str,
) -> dict:
    inputs = _collect_headers(sdk_root, relative_headers)
    manifests = _load_header_shards(header_manifests)
    merged = sdk_header_surface.merge_shard_manifests(
        manifests,
        expected_headers=[display for _, display in inputs],
        target=target,
    )
    print(
        "[sdk-compatibility] verified header shards "
        f"shards={len(manifests)} headers={merged['summary']['header_count']} "
        f"symbols={merged['summary']['unique_symbol_count']}",
        flush=True,
    )
    return merged


def run_sdk_pipeline(
    *,
    sdk_root: Path,
    semantic_providers: Path = DEFAULT_SEMANTIC_PROVIDERS,
    relative_headers: Sequence[str] = (),
    header_manifests: Sequence[Path] = (),
    tapi_targets: Sequence[str] | None = None,
    header_target: str = header_surface.DEFAULT_TARGET,
    clang: str = "clang",
    clang_args: Sequence[str] = (),
    host_target: str = win64_abi_surface.DEFAULT_HOST_TARGET,
    header_jobs: int = sdk_header_surface.DEFAULT_HEADER_JOBS,
    compiler_batch_size: int = compiler_batching.DEFAULT_COMPILER_BATCH_SIZE,
    timeout_seconds: int = 120,
) -> dict:
    """Analyze an SDK root and return the complete in-memory compatibility bundle."""
    sdk_root = _validate_sdk_root(sdk_root)
    if timeout_seconds <= 0:
        raise SdkCompatibilityError("timeout must be positive")
    if compiler_batch_size <= 0:
        raise SdkCompatibilityError("compiler batch size must be positive")
    sdk_header_surface._validate_jobs(header_jobs)
    if shutil.which(clang) is None and not Path(clang).is_file():
        raise SdkCompatibilityError(f"Clang executable not found: {clang}")

    tapi_manifest = _build_tapi_manifest(sdk_root, tapi_targets)
    if header_manifests:
        header_manifest = _merge_header_shards(
            sdk_root,
            relative_headers=relative_headers,
            header_manifests=header_manifests,
            target=header_target,
        )
    else:
        header_manifest = _build_header_manifest(
            sdk_root,
            relative_headers=relative_headers,
            jobs=header_jobs,
            clang=clang,
            target=header_target,
            clang_args=clang_args,
            timeout_seconds=timeout_seconds,
        )
    semantic_manifest = generate_semantic_routes.load_manifest(
        semantic_providers
    )
    bulk = bulk_compatibility.run_pipeline(
        tapi_manifest=tapi_manifest,
        header_manifest=header_manifest,
        semantic_manifest=semantic_manifest,
        header_root=sdk_root,
        sdk_root=sdk_root,
        clang=clang,
        host_target=host_target,
        clang_args=clang_args,
        timeout_seconds=timeout_seconds,
        compiler_batch_size=compiler_batch_size,
    )
    return {
        "tapi_manifest": tapi_manifest,
        "header_manifest": header_manifest,
        **bulk,
    }


def _write_complete_bundle(directory: Path, outputs: dict) -> None:
    directory.mkdir(parents=True, exist_ok=False)
    bulk_compatibility._write_json(
        directory / "tapi-sdk-surface.json",
        outputs["tapi_manifest"],
    )
    bulk_compatibility._write_json(
        directory / "header-signatures.json",
        outputs["header_manifest"],
    )
    bulk_compatibility.write_outputs(directory, outputs)


def write_outputs(output_dir: Path, outputs: dict) -> None:
    """Materialize the complete bundle only after all analysis stages succeeded."""
    destination = output_dir.resolve()
    parent = destination.parent
    parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if not destination.is_dir():
            raise SdkCompatibilityError(
                f"output path exists and is not a directory: {destination.name}"
            )
        if any(destination.iterdir()):
            raise SdkCompatibilityError(
                f"output directory is not empty: {destination.name}"
            )
        destination.rmdir()

    with tempfile.TemporaryDirectory(
        prefix="ipasim-sdk-compatibility-",
        dir=parent,
    ) as temporary:
        staged = Path(temporary) / "bundle"
        _write_complete_bundle(staged, outputs)
        staged.replace(destination)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument(
        "--semantic-providers",
        default=str(DEFAULT_SEMANTIC_PROVIDERS),
        help="explicit semantic-provider inventory JSON",
    )
    parser.add_argument(
        "--relative-header",
        action="append",
        default=[],
        help="optional SDK-relative header subset; repeat as needed",
    )
    parser.add_argument(
        "--header-manifest",
        action="append",
        default=[],
        help=(
            "precomputed exhaustive SDK header shard manifest; repeat for every "
            "shard. Coverage is verified against --sdk-root before use"
        ),
    )
    parser.add_argument(
        "--tapi-target",
        action="append",
        dest="tapi_targets",
        help="TAPI target to include; repeat as needed",
    )
    parser.add_argument(
        "--header-target",
        default=header_surface.DEFAULT_TARGET,
        help=f"Clang iOS target triple (default {header_surface.DEFAULT_TARGET})",
    )
    parser.add_argument("--clang", default=os.environ.get("CLANG", "clang"))
    parser.add_argument("--clang-arg", action="append", default=[])
    parser.add_argument(
        "--host-target",
        default=win64_abi_surface.DEFAULT_HOST_TARGET,
    )
    parser.add_argument(
        "--header-jobs",
        type=int,
        default=sdk_header_surface.DEFAULT_HEADER_JOBS,
        help=(
            "maximum SDK headers analyzed concurrently when no precomputed "
            f"shards are supplied (default {sdk_header_surface.DEFAULT_HEADER_JOBS})"
        ),
    )
    parser.add_argument(
        "--compiler-batch-size",
        type=int,
        default=compiler_batching.DEFAULT_COMPILER_BATCH_SIZE,
        help=(
            "maximum symbols per AAPCS64/Win64 compiler batch "
            f"(default {compiler_batching.DEFAULT_COMPILER_BATCH_SIZE})"
        ),
    )
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args(argv)

    try:
        outputs = run_sdk_pipeline(
            sdk_root=Path(args.sdk_root),
            semantic_providers=Path(args.semantic_providers),
            relative_headers=args.relative_header,
            header_manifests=[Path(item) for item in args.header_manifest],
            tapi_targets=args.tapi_targets,
            header_target=args.header_target,
            clang=args.clang,
            clang_args=args.clang_arg,
            host_target=args.host_target,
            header_jobs=args.header_jobs,
            compiler_batch_size=args.compiler_batch_size,
            timeout_seconds=args.timeout,
        )
        write_outputs(Path(args.output_dir), outputs)
        summary = outputs["compatibility_plan"]["summary"]
        route_counts = summary.get("route_status_counts") or {}
        print(
            "[sdk-compatibility] "
            f"TAPI={outputs['tapi_manifest']['summary']['unique_symbol_count']} "
            f"headers={outputs['header_manifest']['summary']['unique_symbol_count']} "
            f"SDK={summary['symbol_count']} "
            f"typed-C={summary['typed_c_candidate_count']} "
            "route-ready="
            f"{route_counts.get('approved-mechanical-route-ready', 0)}"
        )
        return 0
    except (
        SdkCompatibilityError,
        sdk_header_surface.SdkHeaderSurfaceError,
        tbd_surface.TbdParseError,
        header_surface.HeaderParseError,
        bulk_compatibility.BulkCompatibilityError,
        compiler_batching.CompilerBatchError,
        sdk_catalog.SdkCatalogError,
        abi_surface.AbiSurfaceError,
        win64_abi_surface.Win64AbiError,
        bridge_plan.BridgePlanError,
        runtime_adapter_table.RuntimeAdapterTableError,
        compat_planner.CompatibilityPlannerError,
        generate_semantic_routes.SemanticRouteError,
        OSError,
    ) as exc:
        print(f"[sdk-compatibility] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
