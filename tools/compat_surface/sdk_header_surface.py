#!/usr/bin/env python3
"""Parallel and shardable SDK-scale Clang header signature analysis.

``header_surface.analyze_header`` remains the authoritative per-header parser.
Sharding only partitions the deterministic sorted header inventory. Every shard
still runs the same parser for every header it owns, and merged manifests are
accepted only when their coverage exactly reconstructs the requested SDK
inventory without gaps, overlaps, or target drift.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Sequence, TextIO

import header_surface


DEFAULT_HEADER_JOBS = min(8, max(1, os.cpu_count() or 1))
MAX_HEADER_JOBS = 64
SHARD_COVERAGE_SCHEMA_VERSION = 1
SHARD_STRATEGY = "sorted-round-robin"


class SdkHeaderSurfaceError(ValueError):
    """Raised when SDK-scale header orchestration cannot complete safely."""


def _validate_jobs(jobs: int) -> int:
    if isinstance(jobs, bool) or not isinstance(jobs, int):
        raise SdkHeaderSurfaceError("header jobs must be an integer")
    if jobs <= 0:
        raise SdkHeaderSurfaceError("header jobs must be positive")
    if jobs > MAX_HEADER_JOBS:
        raise SdkHeaderSurfaceError(
            f"header jobs must not exceed {MAX_HEADER_JOBS}"
        )
    return jobs


def _validate_shard(shard_count: int, shard_index: int) -> tuple[int, int]:
    if isinstance(shard_count, bool) or not isinstance(shard_count, int):
        raise SdkHeaderSurfaceError("shard count must be an integer")
    if isinstance(shard_index, bool) or not isinstance(shard_index, int):
        raise SdkHeaderSurfaceError("shard index must be an integer")
    if shard_count <= 0:
        raise SdkHeaderSurfaceError("shard count must be positive")
    if shard_index < 0 or shard_index >= shard_count:
        raise SdkHeaderSurfaceError(
            f"shard index must be in [0, {shard_count - 1}]"
        )
    return shard_count, shard_index


def select_shard(
    inputs: Sequence[tuple[Path, str]],
    *,
    shard_count: int,
    shard_index: int,
) -> list[tuple[Path, str]]:
    """Return one deterministic round-robin partition of sorted inputs."""
    shard_count, shard_index = _validate_shard(shard_count, shard_index)
    ordered = sorted(inputs, key=lambda item: item[1])
    return [
        item
        for index, item in enumerate(ordered)
        if index % shard_count == shard_index
    ]


def _progress_line(
    *,
    completed: int,
    total: int,
    started: float,
    display: str,
) -> str:
    elapsed = max(0.0, time.monotonic() - started)
    rate = completed / elapsed if elapsed > 0 else 0.0
    percent = (100.0 * completed / total) if total else 100.0
    return (
        "[sdk-header-surface] progress "
        f"{completed}/{total} ({percent:.1f}%) "
        f"elapsed={elapsed:.1f}s rate={rate:.2f}/s last={display!r}"
    )


def build_parallel_manifest(
    inputs: Sequence[tuple[Path, str]],
    *,
    jobs: int = DEFAULT_HEADER_JOBS,
    clang: str = "clang",
    target: str = header_surface.DEFAULT_TARGET,
    sdk_root: Path | None = None,
    extra_args: Sequence[str] = (),
    timeout_seconds: int = 120,
    progress_stream: TextIO | None = None,
    progress_every: int = 25,
) -> dict:
    """Analyze inputs concurrently while preserving deterministic results/errors."""
    jobs = _validate_jobs(jobs)
    ordered = list(inputs)
    if not ordered:
        raise SdkHeaderSurfaceError("no headers were supplied")
    if progress_every <= 0:
        raise SdkHeaderSurfaceError("progress interval must be positive")

    signatures_by_index: list[list | None] = [None] * len(ordered)
    stats_by_index: list[dict | None] = [None] * len(ordered)
    errors: list[Exception | None] = [None] * len(ordered)
    started = time.monotonic()

    def analyze(index: int) -> tuple[int, list, dict]:
        path, display = ordered[index]
        signatures, stats = header_surface.analyze_header(
            path,
            display,
            clang=clang,
            target=target,
            sdk_root=sdk_root,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )
        return index, signatures, stats

    worker_count = min(jobs, len(ordered))
    if progress_stream is not None:
        print(
            "[sdk-header-surface] start "
            f"headers={len(ordered)} workers={worker_count} target={target}",
            file=progress_stream,
            flush=True,
        )

    with ThreadPoolExecutor(
        max_workers=worker_count,
        thread_name_prefix="ipasim-sdk-header",
    ) as executor:
        future_to_index = {
            executor.submit(analyze, index): index
            for index in range(len(ordered))
        }
        completed = 0
        for future in as_completed(future_to_index):
            index = future_to_index[future]
            try:
                resolved_index, signatures, stats = future.result()
                if resolved_index != index:
                    raise SdkHeaderSurfaceError(
                        "parallel header worker returned the wrong input index"
                    )
                signatures_by_index[index] = signatures
                stats_by_index[index] = stats
            except Exception as exc:  # inspected deterministically after cleanup
                errors[index] = exc
            completed += 1
            if progress_stream is not None and (
                completed == len(ordered) or completed % progress_every == 0
            ):
                print(
                    _progress_line(
                        completed=completed,
                        total=len(ordered),
                        started=started,
                        display=ordered[index][1],
                    ),
                    file=progress_stream,
                    flush=True,
                )

    # Completion order is deliberately ignored for failure selection. If more
    # than one worker fails, the earliest deterministic SDK input still wins.
    for index, error in enumerate(errors):
        if error is None:
            continue
        display = ordered[index][1]
        if isinstance(error, SdkHeaderSurfaceError):
            raise error
        if isinstance(error, (header_surface.HeaderParseError, OSError)):
            raise SdkHeaderSurfaceError(
                f"header {index + 1}/{len(ordered)} {display!r} failed: {error}"
            ) from error
        raise SdkHeaderSurfaceError(
            f"header {index + 1}/{len(ordered)} {display!r} raised unexpected "
            f"{type(error).__name__}: {error}"
        ) from error

    all_signatures = []
    all_stats = []
    for index in range(len(ordered)):
        signatures = signatures_by_index[index]
        stats = stats_by_index[index]
        if signatures is None or stats is None:
            raise SdkHeaderSurfaceError(
                f"header worker {index + 1}/{len(ordered)} produced no result"
            )
        all_signatures.extend(signatures)
        all_stats.append(stats)

    return header_surface.build_manifest(
        all_signatures,
        target=target,
        headers=[display for _, display in ordered],
        stats=all_stats,
    )


def attach_shard_coverage(
    manifest: dict,
    *,
    all_inputs: Sequence[tuple[Path, str]],
    shard_count: int,
    shard_index: int,
) -> dict:
    """Attach verifiable ownership metadata to a shard manifest."""
    shard_count, shard_index = _validate_shard(shard_count, shard_index)
    selected = select_shard(
        all_inputs,
        shard_count=shard_count,
        shard_index=shard_index,
    )
    result = dict(manifest)
    result["coverage"] = {
        "schema_version": SHARD_COVERAGE_SCHEMA_VERSION,
        "strategy": SHARD_STRATEGY,
        "shard_count": shard_count,
        "shard_index": shard_index,
        "sdk_header_count": len(all_inputs),
        "headers": [display for _, display in selected],
    }
    return result


def _mechanical_signature(item: dict) -> dict:
    parameters = item.get("parameters")
    if not isinstance(parameters, list):
        raise SdkHeaderSurfaceError("header signature parameters must be a list")
    parameter_types = []
    for expected_index, parameter in enumerate(parameters):
        if not isinstance(parameter, dict) or parameter.get("index") != expected_index:
            raise SdkHeaderSurfaceError(
                "header signature parameter indexes must be contiguous and ordered"
            )
        parameter_types.append(parameter.get("type"))
    return {
        "calling_convention": item.get("calling_convention"),
        "variadic": item.get("variadic"),
        "prototype": item.get("prototype"),
        "return_type": item.get("return_type"),
        "parameter_types": parameter_types,
    }


def _merge_signature_group(symbol: str, group: Sequence[dict]) -> dict:
    if not group:
        raise SdkHeaderSurfaceError(f"empty signature group for {symbol}")
    keys = {
        json.dumps(_mechanical_signature(item), sort_keys=True, separators=(",", ":"))
        for item in group
    }
    if len(keys) != 1:
        raise SdkHeaderSurfaceError(
            f"conflicting header signatures across shards for {symbol}"
        )
    first = group[0]
    parameter_count = len(first.get("parameters") or [])
    parameters = []
    for index in range(parameter_count):
        parameters.append(
            {
                "index": index,
                "names": sorted(
                    {
                        name
                        for item in group
                        for name in (item["parameters"][index].get("names") or [])
                    }
                ),
                "spellings": sorted(
                    {
                        spelling
                        for item in group
                        for spelling in (
                            item["parameters"][index].get("spellings") or []
                        )
                    }
                ),
                "type": first["parameters"][index].get("type"),
            }
        )

    sources = {
        (
            source.get("header"),
            source.get("line"),
            source.get("column"),
        )
        for item in group
        for source in (item.get("sources") or [])
        if isinstance(source, dict)
    }
    if any(not isinstance(header, str) or not header for header, _, _ in sources):
        raise SdkHeaderSurfaceError(
            f"header signature {symbol} contains an invalid source header"
        )
    return {
        "symbol": symbol,
        "names": sorted(
            {name for item in group for name in (item.get("names") or [])}
        ),
        "function_type_spellings": sorted(
            {
                spelling
                for item in group
                for spelling in (item.get("function_type_spellings") or [])
            }
        ),
        "calling_convention": first.get("calling_convention"),
        "variadic": first.get("variadic"),
        "prototype": first.get("prototype"),
        "return_type": first.get("return_type"),
        "parameters": parameters,
        "sources": [
            {"header": header, "line": line, "column": column}
            for header, line, column in sorted(
                sources,
                key=lambda value: (value[0], value[1] or 0, value[2] or 0),
            )
        ],
    }


def merge_shard_manifests(
    manifests: Sequence[dict],
    *,
    expected_headers: Sequence[str],
    target: str = header_surface.DEFAULT_TARGET,
) -> dict:
    """Merge exhaustive shard manifests only after proving exact SDK coverage."""
    if not manifests:
        raise SdkHeaderSurfaceError("no header shard manifests were supplied")
    expected = list(expected_headers)
    if len(expected) != len(set(expected)):
        raise SdkHeaderSurfaceError("expected SDK header inventory contains duplicates")

    normalized: dict[int, tuple[dict, dict]] = {}
    declared_shard_count: int | None = None
    for manifest in manifests:
        if not isinstance(manifest, dict):
            raise SdkHeaderSurfaceError("header shard manifest must be an object")
        if manifest.get("schema_version") != 1 or manifest.get("kind") != "header-signature-surface":
            raise SdkHeaderSurfaceError("unsupported header shard manifest schema")
        if manifest.get("target") != target:
            raise SdkHeaderSurfaceError(
                f"header shard target {manifest.get('target')!r} does not match {target!r}"
            )
        coverage = manifest.get("coverage")
        if not isinstance(coverage, dict):
            raise SdkHeaderSurfaceError("header shard manifest has no coverage proof")
        if coverage.get("schema_version") != SHARD_COVERAGE_SCHEMA_VERSION:
            raise SdkHeaderSurfaceError("unsupported header shard coverage schema")
        if coverage.get("strategy") != SHARD_STRATEGY:
            raise SdkHeaderSurfaceError("unsupported header shard partition strategy")
        shard_count = coverage.get("shard_count")
        shard_index = coverage.get("shard_index")
        _validate_shard(shard_count, shard_index)
        if declared_shard_count is None:
            declared_shard_count = shard_count
        elif shard_count != declared_shard_count:
            raise SdkHeaderSurfaceError("header shards disagree on shard count")
        if shard_index in normalized:
            raise SdkHeaderSurfaceError(f"duplicate header shard index {shard_index}")
        normalized[shard_index] = (manifest, coverage)

    assert declared_shard_count is not None
    expected_indices = set(range(declared_shard_count))
    actual_indices = set(normalized)
    if actual_indices != expected_indices:
        missing = sorted(expected_indices - actual_indices)
        extra = sorted(actual_indices - expected_indices)
        raise SdkHeaderSurfaceError(
            f"header shard set is incomplete: missing={missing} extra={extra}"
        )

    signature_groups: dict[str, list[dict]] = {}
    declaration_count = 0
    skipped_cxx = 0
    skipped_static = 0
    owned_headers: set[str] = set()

    for shard_index in range(declared_shard_count):
        manifest, coverage = normalized[shard_index]
        expected_for_shard = expected[shard_index::declared_shard_count]
        headers = coverage.get("headers")
        if not isinstance(headers, list) or not all(
            isinstance(item, str) and item for item in headers
        ):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} coverage headers are invalid"
            )
        if coverage.get("sdk_header_count") != len(expected):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} SDK header count does not match inventory"
            )
        if headers != expected_for_shard:
            missing = sorted(set(expected_for_shard) - set(headers))
            extra = sorted(set(headers) - set(expected_for_shard))
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} coverage mismatch: "
                f"missing={missing[:10]} extra={extra[:10]}"
            )
        overlap = owned_headers.intersection(headers)
        if overlap:
            raise SdkHeaderSurfaceError(
                f"header shard ownership overlaps: {sorted(overlap)[:10]}"
            )
        owned_headers.update(headers)

        summary = manifest.get("summary")
        signatures = manifest.get("signatures")
        if not isinstance(summary, dict) or not isinstance(signatures, list):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} manifest payload is invalid"
            )
        if summary.get("header_count") != len(headers):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} summary header count is inconsistent"
            )
        if summary.get("unique_symbol_count") != len(signatures):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} summary symbol count is inconsistent"
            )
        declaration_count += int(summary.get("declaration_count", 0))
        skipped_cxx += int(summary.get("skipped_cxx_declaration_count", 0))
        skipped_static += int(summary.get("skipped_static_declaration_count", 0))

        shard_header_set = set(headers)
        for item in signatures:
            if not isinstance(item, dict):
                raise SdkHeaderSurfaceError(
                    f"header shard {shard_index} contains a non-object signature"
                )
            symbol = item.get("symbol")
            if not isinstance(symbol, str) or not symbol:
                raise SdkHeaderSurfaceError(
                    f"header shard {shard_index} contains an invalid symbol"
                )
            source_headers = {
                source.get("header")
                for source in (item.get("sources") or [])
                if isinstance(source, dict)
            }
            if not source_headers.issubset(shard_header_set):
                raise SdkHeaderSurfaceError(
                    f"header shard {shard_index} signature {symbol} cites "
                    "a header outside its ownership"
                )
            signature_groups.setdefault(symbol, []).append(item)

    if owned_headers != set(expected):
        missing = sorted(set(expected) - owned_headers)
        extra = sorted(owned_headers - set(expected))
        raise SdkHeaderSurfaceError(
            f"merged header coverage is incomplete: missing={missing[:10]} extra={extra[:10]}"
        )

    signatures = [
        _merge_signature_group(symbol, signature_groups[symbol])
        for symbol in sorted(signature_groups)
    ]
    return {
        "schema_version": 1,
        "kind": "header-signature-surface",
        "target": target,
        "summary": {
            "header_count": len(expected),
            "declaration_count": declaration_count,
            "unique_symbol_count": len(signatures),
            "variadic_symbol_count": sum(
                1 for item in signatures if item.get("variadic")
            ),
            "no_prototype_symbol_count": sum(
                1 for item in signatures if not item.get("prototype")
            ),
            "skipped_cxx_declaration_count": skipped_cxx,
            "skipped_static_declaration_count": skipped_static,
        },
        "signatures": signatures,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("headers", nargs="*", help="explicit header files")
    parser.add_argument(
        "--sdk-root",
        help="SDK root used for includes and relative-path scanning",
    )
    parser.add_argument(
        "--relative-header",
        action="append",
        help="SDK-root-relative header; repeat as needed",
    )
    parser.add_argument(
        "--target",
        default=header_surface.DEFAULT_TARGET,
        help=f"Clang target triple (default: {header_surface.DEFAULT_TARGET})",
    )
    parser.add_argument(
        "--clang",
        default=os.environ.get("CLANG", "clang"),
        help="Clang executable",
    )
    parser.add_argument(
        "--clang-arg",
        action="append",
        default=[],
        help="extra Clang argument; repeat as needed",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="per-Clang-invocation timeout in seconds",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=DEFAULT_HEADER_JOBS,
        help=(
            "maximum headers analyzed concurrently "
            f"(default {DEFAULT_HEADER_JOBS}, maximum {MAX_HEADER_JOBS})"
        ),
    )
    parser.add_argument(
        "--shard-count",
        type=int,
        help="deterministically partition the sorted SDK header inventory",
    )
    parser.add_argument(
        "--shard-index",
        type=int,
        help="zero-based shard to analyze; requires --shard-count",
    )
    parser.add_argument(
        "--progress-every",
        type=int,
        default=25,
        help="emit live progress after this many completed headers",
    )
    parser.add_argument(
        "--output",
        help="write JSON manifest to this file instead of stdout",
    )
    args = parser.parse_args(argv)

    try:
        _validate_jobs(args.jobs)
        if args.timeout <= 0:
            raise SdkHeaderSurfaceError("timeout must be positive")
        if args.progress_every <= 0:
            raise SdkHeaderSurfaceError("progress interval must be positive")
        if (args.shard_count is None) != (args.shard_index is None):
            raise SdkHeaderSurfaceError(
                "--shard-count and --shard-index must be supplied together"
            )
        if shutil.which(args.clang) is None and not Path(args.clang).is_file():
            raise SdkHeaderSurfaceError(
                f"Clang executable not found: {args.clang}"
            )
        all_inputs, sdk_root = header_surface._resolve_inputs(args)
        inputs = all_inputs
        if args.shard_count is not None:
            _validate_shard(args.shard_count, args.shard_index)
            inputs = select_shard(
                all_inputs,
                shard_count=args.shard_count,
                shard_index=args.shard_index,
            )
            if not inputs:
                raise SdkHeaderSurfaceError(
                    f"header shard {args.shard_index}/{args.shard_count} is empty"
                )
            print(
                "[sdk-header-surface] shard "
                f"{args.shard_index + 1}/{args.shard_count} "
                f"owns={len(inputs)} sdk-headers={len(all_inputs)}",
                file=sys.stderr,
                flush=True,
            )
        manifest = build_parallel_manifest(
            inputs,
            jobs=args.jobs,
            clang=args.clang,
            target=args.target,
            sdk_root=sdk_root,
            extra_args=args.clang_arg,
            timeout_seconds=args.timeout,
            progress_stream=sys.stderr,
            progress_every=args.progress_every,
        )
        if args.shard_count is not None:
            manifest = attach_shard_coverage(
                manifest,
                all_inputs=all_inputs,
                shard_count=args.shard_count,
                shard_index=args.shard_index,
            )
        rendered = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (
        SdkHeaderSurfaceError,
        header_surface.HeaderParseError,
        OSError,
    ) as exc:
        print(f"[sdk-header-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
