#!/usr/bin/env python3
"""Run ipaSim's SDK-wide compatibility pipeline in deterministic bounded passes.

This orchestrates the already-separated evidence layers over the complete SDK
surface rather than waiting for a Mach-O runtime failure:

TAPI + Clang -> typed SDK catalog -> AAPCS64 -> Win64 carrier ABI -> libffi plan
-> runtime adapters -> compatibility plan -> approved semantic route table.

The two compiler-backed ABI stages use deterministic bounded batching by default
so SDK-wide runs do not require one monolithic Clang probe. A Mach-O import
manifest is intentionally not an input. Semantic approval remains explicit and
fail-closed; this command only generates a production route when the semantic-
provider inventory already marks it approved.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Sequence

import abi_surface
import bridge_plan
import compat_planner
import compiler_batching
import generate_semantic_routes
import runtime_adapter_table
import sdk_abi_recovery
import sdk_catalog
import win64_abi_surface


class BulkCompatibilityError(ValueError):
    """Raised when the SDK-wide pipeline cannot be completed safely."""


_GUEST_ABI_OPTIMIZATION_FLAGS = {
    "-O0",
    "-O1",
    "-O2",
    "-O3",
    "-Os",
    "-Oz",
    "-Og",
    "-Ofast",
}


def _guest_abi_clang_args(clang_args: Sequence[str]) -> tuple[str, ...]:
    """Use a declaration-canonicalizing Clang pass unless the caller chose one."""
    args = tuple(clang_args)
    if any(argument in _GUEST_ABI_OPTIMIZATION_FLAGS for argument in args):
        return args

    # At -O0 Clang intentionally retains externally linked header-inline bodies as
    # ``available_externally`` LLVM definitions when their address is observed.
    # The SDK ABI probe needs the externally callable function type, not the body.
    # A minimal optimization pass canonicalizes those bodies back to declarations
    # without changing the target ABI lowering. Caller-supplied optimization flags
    # remain authoritative when present.
    return ("-O1", *args)


def _write_json(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )


def _runtime_adapter_symbols(table: dict) -> set[str]:
    if table.get("schema_version") != 1 or table.get("kind") != "runtime-adapter-table":
        raise BulkCompatibilityError(
            "runtime adapter stage did not produce runtime-adapter-table schema version 1"
        )
    symbols: set[str] = set()
    for index, raw in enumerate(table.get("adapters") or []):
        if not isinstance(raw, dict):
            raise BulkCompatibilityError(f"runtime adapters[{index}] must be an object")
        symbol = raw.get("symbol")
        if not isinstance(symbol, str) or not symbol:
            raise BulkCompatibilityError(
                f"runtime adapters[{index}].symbol must be a non-empty string"
            )
        if symbol in symbols:
            raise BulkCompatibilityError(f"runtime adapter table repeats {symbol!r}")
        symbols.add(symbol)
    return symbols


def _validate_approved_routes_have_generated_abi(
    semantic_manifest: dict,
    runtime_adapters: dict,
) -> None:
    available = _runtime_adapter_symbols(runtime_adapters)
    for route in generate_semantic_routes.approved_routes(semantic_manifest):
        adapter = route["adapter_symbol"]
        if adapter not in available:
            raise BulkCompatibilityError(
                f"approved semantic route {route['guest_symbol']!r} has no SDK-wide generated runtime adapter {adapter!r}"
            )


def run_pipeline(
    *,
    tapi_manifest: dict,
    header_manifest: dict,
    semantic_manifest: dict,
    header_root: Path,
    sdk_root: Path | None,
    clang: str,
    host_target: str,
    clang_args: Sequence[str] = (),
    timeout_seconds: int = 120,
    compiler_batch_size: int = compiler_batching.DEFAULT_COMPILER_BATCH_SIZE,
) -> dict:
    """Run all mechanical stages over the SDK and return their in-memory outputs."""
    catalog = sdk_catalog.build_sdk_catalog(tapi_manifest, header_manifest)
    inventory = sdk_catalog.build_abi_inventory(catalog)
    guest_abi = sdk_abi_recovery.build_aapcs64_manifest(
        inventory,
        header_root=header_root,
        clang=clang,
        sdk_root=sdk_root,
        extra_args=_guest_abi_clang_args(clang_args),
        timeout_seconds=timeout_seconds,
        batch_size=compiler_batch_size,
    )
    host_abi = compiler_batching.build_win64_manifest(
        guest_abi,
        clang=clang,
        host_target=host_target,
        extra_args=clang_args,
        timeout_seconds=timeout_seconds,
        batch_size=compiler_batch_size,
    )
    ffi_plan = bridge_plan.build_bridge_plan(host_abi)
    adapters = runtime_adapter_table.build_runtime_adapter_table(ffi_plan)
    _validate_approved_routes_have_generated_abi(semantic_manifest, adapters)
    plan = compat_planner.build_plan(
        catalog,
        semantic_manifest,
        abi_manifest=guest_abi,
    )
    routes_cpp = generate_semantic_routes.render_cpp(semantic_manifest)
    adapters_cpp = runtime_adapter_table.render_cpp_table(
        adapters,
        "makeGeneratedSdkWideAdapterTable",
    )
    return {
        "sdk_catalog": catalog,
        "abi_inventory": inventory,
        "guest_abi": guest_abi,
        "host_abi": host_abi,
        "bridge_plan": ffi_plan,
        "runtime_adapters": adapters,
        "compatibility_plan": plan,
        "routes_cpp": routes_cpp,
        "adapters_cpp": adapters_cpp,
    }


def write_outputs(output_dir: Path, outputs: dict) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    json_outputs = {
        "sdk_catalog": "sdk-catalog.json",
        "abi_inventory": "sdk-abi-inventory.json",
        "guest_abi": "aapcs64-abi.json",
        "host_abi": "win64-abi.json",
        "bridge_plan": "bridge-plan.json",
        "runtime_adapters": "runtime-adapters.json",
        "compatibility_plan": "compatibility-plan.json",
    }
    for key, filename in json_outputs.items():
        _write_json(output_dir / filename, outputs[key])
    (output_dir / "GeneratedSdkAdapters.inc").write_text(
        outputs["adapters_cpp"], encoding="utf-8"
    )
    (output_dir / "ApprovedSemanticImportRoutes.inc").write_text(
        outputs["routes_cpp"], encoding="utf-8"
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tapi", required=True, help="TAPI SDK surface JSON")
    parser.add_argument(
        "--headers", required=True, help="Clang header signature surface JSON"
    )
    parser.add_argument(
        "--semantic-providers",
        required=True,
        help="explicit semantic-provider inventory JSON",
    )
    parser.add_argument(
        "--header-root",
        required=True,
        help="root used to resolve signature source_header paths",
    )
    parser.add_argument("--sdk-root", help="optional Apple SDK root passed as -isysroot")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--clang", default=os.environ.get("CLANG", "clang"))
    parser.add_argument("--clang-arg", action="append", default=[])
    parser.add_argument(
        "--host-target",
        default=win64_abi_surface.DEFAULT_HOST_TARGET,
    )
    parser.add_argument(
        "--compiler-batch-size",
        type=int,
        default=compiler_batching.DEFAULT_COMPILER_BATCH_SIZE,
        help=(
            "maximum symbols per AAPCS64/Win64 Clang probe "
            f"(default {compiler_batching.DEFAULT_COMPILER_BATCH_SIZE})"
        ),
    )
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args(argv)

    try:
        if args.timeout <= 0:
            raise BulkCompatibilityError("timeout must be positive")
        if args.compiler_batch_size <= 0:
            raise BulkCompatibilityError("compiler batch size must be positive")
        if shutil.which(args.clang) is None and not Path(args.clang).is_file():
            raise BulkCompatibilityError(f"Clang executable not found: {args.clang}")
        header_root = Path(args.header_root).resolve()
        if not header_root.is_dir():
            raise BulkCompatibilityError(
                f"header root does not exist: {header_root.name}"
            )
        sdk_root = Path(args.sdk_root).resolve() if args.sdk_root else None
        if sdk_root is not None and not sdk_root.is_dir():
            raise BulkCompatibilityError(f"SDK root does not exist: {sdk_root.name}")

        outputs = run_pipeline(
            tapi_manifest=sdk_catalog.load_manifest(Path(args.tapi), "TAPI"),
            header_manifest=sdk_catalog.load_manifest(Path(args.headers), "header"),
            semantic_manifest=generate_semantic_routes.load_manifest(
                Path(args.semantic_providers)
            ),
            header_root=header_root,
            sdk_root=sdk_root,
            clang=args.clang,
            host_target=args.host_target,
            clang_args=args.clang_arg,
            timeout_seconds=args.timeout,
            compiler_batch_size=args.compiler_batch_size,
        )
        write_outputs(Path(args.output_dir), outputs)
        summary = outputs["compatibility_plan"]["summary"]
        route_counts = summary.get("route_status_counts") or {}
        print(
            "[bulk-compatibility] "
            f"SDK symbols={summary['symbol_count']} "
            f"typed-C={summary['typed_c_candidate_count']} "
            f"compiler-batch-size={args.compiler_batch_size} "
            "route-ready="
            f"{route_counts.get('approved-mechanical-route-ready', 0)}"
        )
        return 0
    except (
        BulkCompatibilityError,
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
        print(f"[bulk-compatibility] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())