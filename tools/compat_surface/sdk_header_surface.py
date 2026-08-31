#!/usr/bin/env python3
"""Parallel SDK-scale orchestration for ipaSim's Clang header signature analyzer.

``header_surface.analyze_header`` remains the authoritative per-header parser.
This command schedules independent SDK headers concurrently, retains every
result in the original deterministic input order, and then calls the existing
``header_surface.build_manifest`` implementation unchanged.

Parallelism changes wall-clock time only. A failed header is never skipped or
converted into a partial successful manifest.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Sequence

import header_surface


DEFAULT_HEADER_JOBS = min(8, max(1, os.cpu_count() or 1))
MAX_HEADER_JOBS = 64


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


def build_parallel_manifest(
    inputs: Sequence[tuple[Path, str]],
    *,
    jobs: int = DEFAULT_HEADER_JOBS,
    clang: str = "clang",
    target: str = header_surface.DEFAULT_TARGET,
    sdk_root: Path | None = None,
    extra_args: Sequence[str] = (),
    timeout_seconds: int = 120,
) -> dict:
    """Analyze sorted header inputs concurrently and return the normal manifest."""
    jobs = _validate_jobs(jobs)
    ordered = list(inputs)
    if not ordered:
        raise SdkHeaderSurfaceError("no headers were supplied")

    signatures_by_index: list[list | None] = [None] * len(ordered)
    stats_by_index: list[dict | None] = [None] * len(ordered)
    errors: list[BaseException | None] = [None] * len(ordered)

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
    with ThreadPoolExecutor(
        max_workers=worker_count,
        thread_name_prefix="ipasim-sdk-header",
    ) as executor:
        futures = [executor.submit(analyze, index) for index in range(len(ordered))]
        # Resolve futures in deterministic input order rather than completion
        # order. If several headers fail concurrently, the same earliest SDK
        # header is always reported.
        for index, future in enumerate(futures):
            try:
                resolved_index, signatures, stats = future.result()
                if resolved_index != index:
                    raise SdkHeaderSurfaceError(
                        "parallel header worker returned the wrong input index"
                    )
                signatures_by_index[index] = signatures
                stats_by_index[index] = stats
            except BaseException as exc:  # inspect deterministically after cleanup
                errors[index] = exc

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
        "--output",
        help="write JSON manifest to this file instead of stdout",
    )
    args = parser.parse_args(argv)

    try:
        _validate_jobs(args.jobs)
        if args.timeout <= 0:
            raise SdkHeaderSurfaceError("timeout must be positive")
        if shutil.which(args.clang) is None and not Path(args.clang).is_file():
            raise SdkHeaderSurfaceError(
                f"Clang executable not found: {args.clang}"
            )
        inputs, sdk_root = header_surface._resolve_inputs(args)
        manifest = build_parallel_manifest(
            inputs,
            jobs=args.jobs,
            clang=args.clang,
            target=args.target,
            sdk_root=sdk_root,
            extra_args=args.clang_arg,
            timeout_seconds=args.timeout,
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
