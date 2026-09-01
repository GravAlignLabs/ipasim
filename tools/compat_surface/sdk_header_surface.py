#!/usr/bin/env python3
"""Parallel, contextual, and shardable SDK-scale header signature analysis.

``header_surface`` remains authoritative for Clang AST/type canonicalization.
This SDK layer supplies the compilation context that Apple framework headers
expect while keeping each physical header as the authoritative source being
measured. Framework leaves are parsed through a wrapper translation unit that
loads the framework umbrella before the target leaf, then Clang declarations
are attributed back to the physical target header by source location.

Declarations that Clang proves are unavailable for the selected target remain
explicit evidence in the manifest rather than being silently discarded.
Sharding only partitions the deterministic sorted physical-header inventory;
merged manifests are accepted only when coverage exactly reconstructs that
inventory without gaps, overlaps, or target drift.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Sequence, TextIO

import header_surface


DEFAULT_HEADER_JOBS = min(8, max(1, os.cpu_count() or 1))
MAX_HEADER_JOBS = 64
SHARD_COVERAGE_SCHEMA_VERSION = 1
SHARD_STRATEGY = "sorted-round-robin"
_UNAVAILABLE_DIAGNOSTIC = re.compile(
    r"error:\s+'([^']+)'\s+is unavailable(?:\s*:\s*(.*))?$",
    re.IGNORECASE,
)


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


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _framework_root(path: Path, sdk_root: Path) -> Path | None:
    """Return the innermost containing .framework directory, if any."""
    resolved = path.resolve()
    root = sdk_root.resolve()
    if not _is_within(resolved, root):
        return None
    for parent in resolved.parents:
        if parent.name.endswith(".framework"):
            return parent
        if parent == root:
            break
    return None


def _framework_context_header(path: Path, sdk_root: Path) -> Path | None:
    framework = _framework_root(path, sdk_root)
    if framework is None:
        return None
    name = framework.name[: -len(".framework")]
    candidates = [
        framework / "Headers" / f"{name}.h",
        framework / "PrivateHeaders" / f"{name}.h",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return path.resolve()


def _framework_search_paths(path: Path, sdk_root: Path) -> list[Path]:
    """Return framework search roots in context-first precedence order."""
    resolved = path.resolve()
    root = sdk_root.resolve()
    candidates: list[Path] = []

    # The nearest framework search root must win. This is required for nested
    # frameworks (for example Accelerate/vecLib), Developer frameworks, and
    # Cryptex overlays that can share names with ordinary system frameworks.
    for parent in resolved.parents:
        if parent.name in ("Frameworks", "PrivateFrameworks") and _is_within(
            parent, root
        ):
            candidates.append(parent)
        if parent == root:
            break

    candidates.extend(
        [
            root / "Developer" / "Library" / "Frameworks",
            root / "System" / "Cryptexes" / "OS" / "System" / "Library" / "Frameworks",
            root / "System" / "Cryptexes" / "OS" / "System" / "Library" / "PrivateFrameworks",
            root / "System" / "Library" / "Frameworks",
            root / "System" / "Library" / "PrivateFrameworks",
        ]
    )

    result: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        if not candidate.is_dir():
            continue
        key = os.path.normcase(str(candidate.resolve()))
        if key in seen:
            continue
        seen.add(key)
        result.append(candidate.resolve())
    return result


def _sdk_clang_base(
    clang: str,
    target: str,
    language: str,
    sdk_root: Path,
    header_path: Path,
    extra_args: Sequence[str],
) -> list[str]:
    """Build the Clang baseline with SDK-context framework precedence."""
    root = sdk_root.resolve()
    args = [
        clang,
        "-target",
        target,
        "-x",
        language,
        "-fsyntax-only",
        "-fno-builtin",
        "-fblocks",
        "-Wno-everything",
        "-ferror-limit=50",
        "-isysroot",
        str(root),
    ]
    usr_include = root / "usr" / "include"
    if usr_include.is_dir():
        args += ["-isystem", str(usr_include)]
    for framework_path in _framework_search_paths(header_path, root):
        args += ["-F", str(framework_path)]
    args += list(extra_args)
    return args


def _quote_import(path: Path) -> str:
    return str(path.resolve()).replace("\\", "/").replace('"', '\\"')


def _context_source(context_header: Path, target_header: Path) -> str:
    lines = [f'#import "{_quote_import(context_header)}"']
    if context_header.resolve() != target_header.resolve():
        lines.append(f'#import "{_quote_import(target_header)}"')
    return "\n".join(lines) + "\n"


def _location_candidates(loc: dict) -> list[dict]:
    """Return Clang source-location variants without treating includers as owners."""
    result = []
    for candidate in (
        loc,
        loc.get("expansionLoc") or {},
        loc.get("spellingLoc") or {},
    ):
        if isinstance(candidate, dict):
            result.append(candidate)
    return result


def _normalized_source_file(value) -> str | None:
    if not isinstance(value, str) or not value:
        return None
    return os.path.normcase(os.path.abspath(value))


def _source_line_column(source: bytes, offset: int) -> tuple[int, int]:
    prefix = source[:offset]
    line = prefix.count(b"\n") + 1
    last_newline = prefix.rfind(b"\n")
    column = offset + 1 if last_newline < 0 else offset - last_newline
    return line, column


def _node_belongs_to_source(
    node: dict,
    *,
    source_path: Path,
    source_bytes: bytes,
) -> bool:
    """Attribute a Clang declaration to one physical header, fail-closed.

    Clang JSON elides the ``file`` field when a declaration continues in the
    same source file as nearby AST records. Explicit file locations therefore
    win first. When the file is elided, the declaration is accepted only when
    its byte offset, token spelling, and any emitted line/column coordinates
    all verify against the target header itself. ``includedFrom`` is never used
    as ownership evidence because it names the includer, not the declaration.
    """
    loc = node.get("loc") or {}
    if not isinstance(loc, dict):
        return False
    candidates = _location_candidates(loc)
    expected_file = os.path.normcase(
        os.path.abspath(str(source_path.resolve()))
    )
    explicit_files = {
        normalized
        for candidate in candidates
        for normalized in [_normalized_source_file(candidate.get("file"))]
        if normalized is not None
    }
    if expected_file in explicit_files:
        return True
    if explicit_files:
        return False

    name = node.get("name")
    if not isinstance(name, str) or not name:
        return False
    name_bytes = name.encode("utf-8")
    for candidate in candidates:
        offset = candidate.get("offset")
        if isinstance(offset, bool) or not isinstance(offset, int) or offset < 0:
            continue
        token_length = candidate.get("tokLen")
        if token_length is None:
            token_length = len(name_bytes)
        if (
            isinstance(token_length, bool)
            or not isinstance(token_length, int)
            or token_length <= 0
        ):
            continue
        end = offset + token_length
        if end > len(source_bytes):
            continue
        if source_bytes[offset:end] != name_bytes:
            continue
        expected_line, expected_column = _source_line_column(source_bytes, offset)
        line = candidate.get("line")
        column = candidate.get("col")
        if line is not None and (
            isinstance(line, bool)
            or not isinstance(line, int)
            or line != expected_line
        ):
            continue
        if column is not None and (
            isinstance(column, bool)
            or not isinstance(column, int)
            or column != expected_column
        ):
            continue
        return True
    return False


def _discover_raw_functions_for_source(
    ast: dict,
    *,
    header: str,
    source_path: Path,
) -> tuple[list[header_surface.RawFunction], int, int]:
    """Discover C functions physically declared by one target header only."""
    source_bytes = source_path.read_bytes()
    functions: list[header_surface.RawFunction] = []
    skipped_cxx = 0
    skipped_static = 0
    for node, ancestors in header_surface._walk(ast):
        if node.get("kind") != "FunctionDecl":
            continue
        if node.get("isImplicit"):
            continue
        if not _node_belongs_to_source(
            node,
            source_path=source_path,
            source_bytes=source_bytes,
        ):
            continue
        if any(kind in header_surface.SCOPE_BLOCKERS for kind in ancestors):
            skipped_cxx += 1
            continue
        if node.get("storageClass") == "static":
            skipped_static += 1
            continue
        name = node.get("name")
        mangled = node.get("mangledName")
        function_type = (node.get("type") or {}).get("qualType")
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(mangled, str)
            or not mangled
        ):
            continue
        if mangled.startswith("__Z"):
            skipped_cxx += 1
            continue
        if not isinstance(function_type, str) or not function_type:
            raise header_surface.HeaderParseError(
                f"{header}: function {name} has no Clang type spelling"
            )
        params = []
        for child in node.get("inner") or []:
            if (
                not isinstance(child, dict)
                or child.get("kind") != "ParmVarDecl"
            ):
                continue
            spelling = (child.get("type") or {}).get("qualType")
            if not isinstance(spelling, str) or not spelling:
                raise header_surface.HeaderParseError(
                    f"{header}: parameter of {name} has no type spelling"
                )
            params.append(
                header_surface.RawParam(child.get("name"), spelling)
            )
        line, column = header_surface._source_position(node.get("loc") or {})
        functions.append(
            header_surface.RawFunction(
                header=header,
                name=name,
                symbol=mangled,
                function_type=function_type,
                params=tuple(params),
                variadic=bool(node.get("variadic", False)),
                line=line,
                column=column,
            )
        )
    return functions, skipped_cxx, skipped_static


def _context_helper_source(
    context_header: Path,
    target_header: Path,
    functions: Sequence[header_surface.RawFunction],
) -> tuple[str, dict[str, header_surface.RawFunction]]:
    helper_text, mapping = header_surface._helper_source(target_header, functions)
    helper_lines = helper_text.splitlines()
    imports = _context_source(context_header, target_header).rstrip("\n").splitlines()
    # _helper_source begins by including target_header. Replace that one include
    # with the context imports while retaining its authoritative typedef probes.
    return "\n".join(imports + helper_lines[1:]) + "\n", mapping


def _target_unavailable_diagnostics(message: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in message.splitlines():
        match = _UNAVAILABLE_DIAGNOSTIC.search(line.strip())
        if not match:
            continue
        name = match.group(1)
        reason = (match.group(2) or "unavailable for selected target").strip()
        result.setdefault(name, reason)
    return result


def _unavailable_record(
    function: header_surface.RawFunction,
    reason: str,
) -> dict:
    return {
        "symbol": function.symbol,
        "name": function.name,
        "function_type_spelling": function.function_type,
        "reason": reason,
        "source": {
            "header": function.header,
            "line": function.line,
            "column": function.column,
        },
    }


def _recover_contextual_signatures(
    *,
    raw: Sequence[header_surface.RawFunction],
    context_header: Path,
    target_header: Path,
    display: str,
    clang: str,
    target: str,
    sdk_root: Path,
    extra_args: Sequence[str],
    replacements: Sequence[tuple[str, str]],
    timeout_seconds: int,
) -> tuple[list[header_surface.HeaderSignature], list[dict]]:
    remaining = list(raw)
    unavailable: list[dict] = []
    second_output = ""
    helper_map: dict[str, header_surface.RawFunction] = {}

    with tempfile.TemporaryDirectory(prefix="ipasim-sdk-header-surface-") as directory:
        helper_path = Path(directory) / "signature_probe.m"
        helper_replacements = list(replacements) + [(directory, "<TMP>")]

        while remaining:
            helper_text, helper_map = _context_helper_source(
                context_header,
                target_header,
                remaining,
            )
            helper_path.write_text(helper_text, encoding="utf-8")
            second_args = _sdk_clang_base(
                clang,
                target,
                "objective-c",
                sdk_root,
                target_header,
                extra_args,
            )
            second_args += [
                "-Xclang",
                "-ast-dump-all=json",
                "-Xclang",
                "-ast-dump-filter",
                "-Xclang",
                "__ipasim_signature_",
                str(helper_path),
            ]
            try:
                second_output = header_surface._run_clang(
                    second_args,
                    helper_replacements,
                    timeout_seconds,
                )
            except header_surface.HeaderParseError as exc:
                diagnostics = _target_unavailable_diagnostics(str(exc))
                remaining_names = {function.name for function in remaining}
                matched_names = remaining_names.intersection(diagnostics)
                if not matched_names:
                    raise
                for function in remaining:
                    if function.name in matched_names:
                        unavailable.append(
                            _unavailable_record(
                                function,
                                diagnostics[function.name],
                            )
                        )
                remaining = [
                    function
                    for function in remaining
                    if function.name not in matched_names
                ]
                helper_map = {}
                second_output = ""
                continue
            break

    if not remaining:
        return [], unavailable

    typedefs = {}
    for obj in header_surface._json_objects(second_output):
        if obj.get("kind") == "TypedefDecl" and obj.get("name") in helper_map:
            typedefs[obj["name"]] = obj
    missing = sorted(set(helper_map) - set(typedefs))
    if missing:
        raise header_surface.HeaderParseError(
            f"{display}: Clang did not emit helper type metadata for {missing[:5]}"
        )

    recovered_by_name: dict[
        str, tuple[dict, str, bool, bool, tuple[dict, ...]]
    ] = {}
    for helper, function in helper_map.items():
        fn_type = header_surface._find_function_type(typedefs[helper])
        if fn_type is None:
            raise header_surface.HeaderParseError(
                f"{display}: helper for {function.name} has no function type"
            )
        children = header_surface._type_children(fn_type)
        if not children:
            raise header_surface.HeaderParseError(
                f"{display}: helper for {function.name} has no return type"
            )
        prototype = fn_type.get("kind") == "FunctionProtoType"
        variadic = bool(fn_type.get("variadic", False))
        param_types = tuple(
            header_surface._type_descriptor(item) for item in children[1:]
        )
        if prototype and len(param_types) != len(function.params):
            raise header_surface.HeaderParseError(
                f"{display}: Clang type tree for {function.name} has "
                f"{len(param_types)} parameters but declaration has "
                f"{len(function.params)}"
            )
        recovered_by_name[function.name] = (
            header_surface._type_descriptor(children[0]),
            str(fn_type.get("cc", "cdecl")),
            variadic,
            prototype,
            param_types,
        )

    signatures: list[header_surface.HeaderSignature] = []
    for function in remaining:
        return_type, cc, variadic, prototype, param_types = recovered_by_name[
            function.name
        ]
        if variadic != function.variadic:
            raise header_surface.HeaderParseError(
                f"{display}: inconsistent variadic metadata for {function.name}"
            )
        signatures.append(
            header_surface.HeaderSignature(
                header=display,
                name=function.name,
                symbol=function.symbol,
                function_type=function.function_type,
                calling_convention=cc,
                variadic=variadic,
                prototype=prototype,
                return_type=return_type,
                parameter_types=param_types,
                params=function.params,
                line=function.line,
                column=function.column,
            )
        )
    return signatures, unavailable


def _analyze_sdk_header(
    path: Path,
    display: str,
    *,
    clang: str,
    target: str,
    sdk_root: Path | None,
    extra_args: Sequence[str],
    timeout_seconds: int,
) -> tuple[list[header_surface.HeaderSignature], dict]:
    # Preserve the original parser byte-for-byte for ordinary non-framework
    # headers. SDK context reconstruction is applied only to .framework leaves.
    if sdk_root is None:
        return header_surface.analyze_header(
            path,
            display,
            clang=clang,
            target=target,
            sdk_root=sdk_root,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )

    resolved = path.resolve()
    root = sdk_root.resolve()
    framework = _framework_root(resolved, root)
    if framework is None:
        return header_surface.analyze_header(
            resolved,
            display,
            clang=clang,
            target=target,
            sdk_root=root,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )
    if not resolved.is_file():
        raise header_surface.HeaderParseError(f"header does not exist: {display}")

    context_header = _framework_context_header(resolved, root) or resolved
    replacements = [(str(resolved), display), (str(root), "<SDKROOT>")]
    if context_header != resolved:
        replacements.append((str(context_header), "<FRAMEWORK-CONTEXT>"))

    # Parse a normal wrapper translation unit instead of making the target leaf
    # Clang's main file. A main-file leaf is considered already entered while
    # -include processes its umbrella, so a sibling that recursively imports
    # that leaf can observe missing declarations. The wrapper lets Apple's own
    # import graph execute normally; source attribution below still limits the
    # manifest to declarations physically owned by this exact target header.
    with tempfile.TemporaryDirectory(
        prefix="ipasim-sdk-header-context-"
    ) as directory:
        source_path = Path(directory) / "header_context.m"
        source_path.write_text(
            _context_source(context_header, resolved),
            encoding="utf-8",
        )
        first_replacements = replacements + [(directory, "<TMP>")]
        first_args = _sdk_clang_base(
            clang,
            target,
            "objective-c",
            root,
            resolved,
            extra_args,
        )
        first_args += ["-Xclang", "-ast-dump=json", str(source_path)]
        first_output = header_surface._run_clang(
            first_args,
            first_replacements,
            timeout_seconds,
        )

    objects = header_surface._json_objects(first_output)
    if len(objects) != 1:
        raise header_surface.HeaderParseError(
            f"{display}: expected one Clang translation-unit AST, got {len(objects)}"
        )
    raw, skipped_cxx, skipped_static = _discover_raw_functions_for_source(
        objects[0],
        header=display,
        source_path=resolved,
    )
    if not raw:
        return [], {
            "skipped_cxx": skipped_cxx,
            "skipped_static": skipped_static,
            "declarations": 0,
        }

    signatures, unavailable = _recover_contextual_signatures(
        raw=raw,
        context_header=context_header,
        target_header=resolved,
        display=display,
        clang=clang,
        target=target,
        sdk_root=root,
        extra_args=extra_args,
        replacements=replacements,
        timeout_seconds=timeout_seconds,
    )
    stats = {
        "skipped_cxx": skipped_cxx,
        "skipped_static": skipped_static,
        "declarations": len(raw),
    }
    if unavailable:
        stats["target_unavailable"] = unavailable
    return signatures, stats


def _sorted_unavailable(records: Sequence[dict]) -> list[dict]:
    return sorted(
        records,
        key=lambda item: (
            (item.get("source") or {}).get("header") or "",
            (item.get("source") or {}).get("line") or 0,
            (item.get("source") or {}).get("column") or 0,
            item.get("symbol") or "",
            item.get("name") or "",
        ),
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
        signatures, stats = _analyze_sdk_header(
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
    unavailable = []
    for index in range(len(ordered)):
        signatures = signatures_by_index[index]
        stats = stats_by_index[index]
        if signatures is None or stats is None:
            raise SdkHeaderSurfaceError(
                f"header worker {index + 1}/{len(ordered)} produced no result"
            )
        all_signatures.extend(signatures)
        all_stats.append(stats)
        unavailable.extend(stats.get("target_unavailable") or [])

    manifest = header_surface.build_manifest(
        all_signatures,
        target=target,
        headers=[display for _, display in ordered],
        stats=all_stats,
    )
    if unavailable:
        rendered_unavailable = _sorted_unavailable(unavailable)
        manifest["summary"]["target_unavailable_declaration_count"] = len(
            rendered_unavailable
        )
        manifest["target_unavailable"] = rendered_unavailable
    return manifest


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


def _validate_unavailable_record(
    item: dict,
    *,
    shard_index: int,
    shard_headers: set[str],
) -> None:
    if not isinstance(item, dict):
        raise SdkHeaderSurfaceError(
            f"header shard {shard_index} contains a non-object unavailable record"
        )
    if not isinstance(item.get("symbol"), str) or not item.get("symbol"):
        raise SdkHeaderSurfaceError(
            f"header shard {shard_index} contains an invalid unavailable symbol"
        )
    if not isinstance(item.get("name"), str) or not item.get("name"):
        raise SdkHeaderSurfaceError(
            f"header shard {shard_index} contains an invalid unavailable name"
        )
    source = item.get("source")
    if not isinstance(source, dict) or source.get("header") not in shard_headers:
        raise SdkHeaderSurfaceError(
            f"header shard {shard_index} unavailable declaration cites "
            "a header outside its ownership"
        )
    if not isinstance(item.get("reason"), str) or not item.get("reason"):
        raise SdkHeaderSurfaceError(
            f"header shard {shard_index} unavailable declaration has no reason"
        )


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
        if (
            manifest.get("schema_version") != 1
            or manifest.get("kind") != "header-signature-surface"
        ):
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
    unavailable_records: list[dict] = []
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

        shard_unavailable = manifest.get("target_unavailable") or []
        if not isinstance(shard_unavailable, list):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} unavailable payload is invalid"
            )
        declared_unavailable = int(
            summary.get("target_unavailable_declaration_count", 0)
        )
        if declared_unavailable != len(shard_unavailable):
            raise SdkHeaderSurfaceError(
                f"header shard {shard_index} unavailable count is inconsistent"
            )
        for item in shard_unavailable:
            _validate_unavailable_record(
                item,
                shard_index=shard_index,
                shard_headers=shard_header_set,
            )
            unavailable_records.append(item)

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
    result = {
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
    if unavailable_records:
        rendered_unavailable = _sorted_unavailable(unavailable_records)
        result["summary"]["target_unavailable_declaration_count"] = len(
            rendered_unavailable
        )
        result["target_unavailable"] = rendered_unavailable
    return result


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
