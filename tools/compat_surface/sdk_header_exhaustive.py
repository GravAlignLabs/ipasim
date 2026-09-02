#!/usr/bin/env python3
"""Exhaustive contextual SDK header analysis for ipaSim.

``sdk_header_surface`` remains the authoritative manifest/merge implementation.
This driver supplies the compilation-context recovery required by a complete
physical-header pass: SDK-declared umbrellas, source-language fallbacks,
Swift-importer branches, actual Clang module imports, reverse include owners,
lexical prerequisite providers, and explicit non-surface evidence.

No header is removed from coverage. A physical header either contributes typed
C declarations, contributes explicit target-unavailable declarations, is
recorded as target-inactive with SDK evidence, or fails the preflight.
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
import sdk_header_context
import sdk_header_surface as core

DEFAULT_HEADER_JOBS = core.DEFAULT_HEADER_JOBS
MAX_HEADER_JOBS = core.MAX_HEADER_JOBS
SdkHeaderSurfaceError = core.SdkHeaderSurfaceError
_validate_jobs = core._validate_jobs
_validate_shard = core._validate_shard
select_shard = core.select_shard
attach_shard_coverage = core.attach_shard_coverage
merge_shard_manifests = core.merge_shard_manifests


def _context_extra_args(
    target_header: Path,
    context_header: Path,
    sdk_root: Path,
    extra_args: Sequence[str],
) -> tuple[str, ...]:
    """Return only compilation flags justified by the SDK header context."""
    result = list(extra_args)
    if (
        sdk_header_context.requires_clang_modules(target_header)
        or sdk_header_context.requires_clang_modules(context_header)
    ) and "-fmodules" not in result:
        result.append("-fmodules")
    for header in (context_header, target_header):
        for argument in sdk_header_context.swift_importer_args(header, sdk_root):
            if argument not in result:
                result.append(argument)
    return tuple(result)


def _context_for_header(path: Path, sdk_root: Path) -> tuple[Path | None, str]:
    language = sdk_header_context.preferred_clang_language(path)
    framework_context = core._framework_context_header(path, sdk_root)
    if framework_context is not None:
        return framework_context, language
    if core._is_libcxx_header(path, sdk_root):
        return path.resolve(), "objective-c++"
    module_context = core._module_umbrella_context_header(path, sdk_root)
    if module_context is not None:
        return module_context, language
    return None, language


def _language_attempts(
    preferred: str,
    *,
    target_header: Path,
    sdk_root: Path,
) -> tuple[str, ...]:
    """Try the SDK-declared language first, then a C ABI-compatible fallback.

    Swift overlay shims commonly carry a C++ editor modeline even when their C
    ABI declarations are intentionally consumable by Clang's C importer. If a
    non-libc++ header fails only under C++, trying Objective-C is safe because
    Clang still has to parse the exact physical header and source ownership is
    unchanged. libc++ leaves never receive this fallback.
    """
    result = [preferred]
    if preferred.endswith("++") and not core._is_libcxx_header(target_header, sdk_root):
        result.append("objective-c")
    return tuple(dict.fromkeys(result))


def _candidate_languages(
    target_language: str,
    context_header: Path,
    *,
    target_header: Path,
    sdk_root: Path,
) -> tuple[str, ...]:
    preferred = target_language
    if sdk_header_context.preferred_clang_language(context_header).endswith("++"):
        preferred = "objective-c++"
    return _language_attempts(
        preferred,
        target_header=target_header,
        sdk_root=sdk_root,
    )


def _is_libcxx_support_header(path: Path, sdk_root: Path) -> bool:
    """Return whether a header is an internal libc++ ``__support`` leaf."""
    libcxx = core._libcxx_root(sdk_root)
    if libcxx is None:
        return False
    try:
        relative = path.resolve().relative_to(libcxx.resolve())
    except ValueError:
        return False
    return len(relative.parts) >= 3 and relative.parts[0] == "__support"


def _discover_raw_functions_for_source(
    ast: dict,
    *,
    header: str,
    source_path: Path,
) -> tuple[list[header_surface.RawFunction], int, int]:
    """Discover global C functions physically declared by one target header."""
    source_bytes = source_path.read_bytes()
    functions: list[header_surface.RawFunction] = []
    skipped_cxx = 0
    skipped_static = 0
    for node, ancestors in header_surface._walk(ast):
        if node.get("kind") != "FunctionDecl":
            continue
        if node.get("isImplicit"):
            continue
        if not core._node_belongs_to_source(
            node,
            source_path=source_path,
            source_bytes=source_bytes,
        ):
            continue
        # Local extern declarations inside inline/helper functions are not
        # global header ABI surface. The FunctionDecl ancestor distinguishes
        # them without excluding the top-level FunctionDecl itself.
        if "FunctionDecl" in ancestors:
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
            if not isinstance(child, dict) or child.get("kind") != "ParmVarDecl":
                continue
            spelling = (child.get("type") or {}).get("qualType")
            if not isinstance(spelling, str) or not spelling:
                raise header_surface.HeaderParseError(
                    f"{header}: parameter of {name} has no type spelling"
                )
            params.append(header_surface.RawParam(child.get("name"), spelling))
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
    if not resolved.is_file():
        raise header_surface.HeaderParseError(f"header does not exist: {display}")

    context_header, language = _context_for_header(resolved, root)
    original_error: header_surface.HeaderParseError | None = None
    modules_required = sdk_header_context.requires_clang_modules(resolved)
    swift_importer_required = bool(sdk_header_context.swift_importer_args(resolved, root))
    if (
        context_header is None
        and language == "objective-c"
        and not modules_required
        and not swift_importer_required
    ):
        try:
            return header_surface.analyze_header(
                resolved,
                display,
                clang=clang,
                target=target,
                sdk_root=root,
                extra_args=extra_args,
                timeout_seconds=timeout_seconds,
            )
        except header_surface.HeaderParseError as exc:
            original_error = exc
            context_header = resolved
    elif context_header is None:
        context_header = resolved

    assert context_header is not None
    replacements = [(str(resolved), display), (str(root), "<SDKROOT>")]
    initial_errors: list[header_surface.HeaderParseError] = []
    ast: dict | None = None
    target_entered = False
    active_extra_args: tuple[str, ...] = ()

    for attempted_language in _language_attempts(
        language,
        target_header=resolved,
        sdk_root=root,
    ):
        attempted_args = _context_extra_args(
            resolved,
            context_header,
            root,
            extra_args,
        )
        try:
            attempted_ast, attempted_entered = core._parse_context_translation_unit(
                context_header=context_header,
                target_header=resolved,
                include_target=True,
                display=display,
                clang=clang,
                target=target,
                sdk_root=root,
                language=attempted_language,
                extra_args=attempted_args,
                replacements=replacements,
                timeout_seconds=timeout_seconds,
            )
        except header_surface.HeaderParseError as exc:
            initial_errors.append(exc)
            continue
        ast = attempted_ast
        target_entered = attempted_entered
        language = attempted_language
        active_extra_args = attempted_args
        break

    if ast is None:
        context_error = initial_errors[0]
        libcxx = core._libcxx_root(root)
        candidates: list[tuple[Path, bool]] = []
        seen_candidates: set[tuple[str, bool]] = set()

        def add_candidate(candidate: Path, include_target: bool) -> None:
            candidate = candidate.resolve()
            key = (os.path.normcase(str(candidate)), include_target)
            if candidate == resolved or key in seen_candidates:
                return
            seen_candidates.add(key)
            candidates.append((candidate, include_target))

        diagnostic_messages = [str(error) for error in initial_errors]
        if original_error is not None:
            diagnostic_messages.append(str(original_error))

        for message in diagnostic_messages:
            for candidate in sdk_header_context.recommended_context_headers(
                message,
                target_header=resolved,
                sdk_root=root,
                libcxx_root=libcxx,
            ):
                # First honor "include X instead" as a real owner. If X does
                # not enter the leaf, retain that as explicit inactivity
                # evidence, then try X as a prerequisite for legacy headers.
                add_candidate(candidate, False)
                add_candidate(candidate, True)

        for candidate in sdk_header_context.reverse_context_candidates(
            resolved,
            sdk_root=root,
            libcxx_root=libcxx,
        ):
            add_candidate(candidate, False)

        for message in diagnostic_messages:
            for candidate in sdk_header_context.definition_provider_candidates(
                message,
                target_header=resolved,
                sdk_root=root,
            ):
                add_candidate(candidate, True)

        selected_ast: dict | None = None
        selected_context: Path | None = None
        selected_language = language
        selected_extra_args: tuple[str, ...] = ()
        inactive_context: Path | None = None
        candidate_errors: list[str] = []

        for candidate, include_target in candidates:
            candidate_args = _context_extra_args(
                resolved,
                candidate,
                root,
                extra_args,
            )
            for candidate_language in _candidate_languages(
                language,
                candidate,
                target_header=resolved,
                sdk_root=root,
            ):
                try:
                    candidate_ast, candidate_entered = core._parse_context_translation_unit(
                        context_header=candidate,
                        target_header=resolved,
                        include_target=include_target,
                        display=display,
                        clang=clang,
                        target=target,
                        sdk_root=root,
                        language=candidate_language,
                        extra_args=candidate_args,
                        replacements=replacements,
                        timeout_seconds=timeout_seconds,
                    )
                except header_surface.HeaderParseError as exc:
                    candidate_errors.append(str(exc))
                    continue
                if candidate_entered:
                    selected_ast = candidate_ast
                    selected_context = candidate
                    selected_language = candidate_language
                    selected_extra_args = candidate_args
                    break
                if inactive_context is None:
                    inactive_context = candidate
            if selected_ast is not None:
                break

        if selected_ast is not None and selected_context is not None:
            ast = selected_ast
            context_header = selected_context
            language = selected_language
            active_extra_args = selected_extra_args
            target_entered = True
        elif inactive_context is not None:
            return [], {
                "skipped_cxx": 0,
                "skipped_static": 0,
                "declarations": 0,
                "target_inactive": core._target_inactive_record(
                    display=display,
                    sdk_root=root,
                    context_header=inactive_context,
                    reason=(
                        "SDK include owner compiles for the selected target but does "
                        "not activate this physical header"
                    ),
                ),
            }
        else:
            inactive_reason = sdk_header_context.explicit_inactive_reason(
                "\n".join(diagnostic_messages + candidate_errors),
                target_header=resolved,
                sdk_root=root,
                libcxx_root=libcxx,
            )
            if inactive_reason is not None:
                return [], {
                    "skipped_cxx": 0,
                    "skipped_static": 0,
                    "declarations": 0,
                    "target_inactive": core._target_inactive_record(
                        display=display,
                        sdk_root=root,
                        context_header=None,
                        reason=inactive_reason,
                    ),
                }
            if sdk_header_context.is_unreferenced_libcxx_support_header(
                resolved,
                sdk_root=root,
                libcxx_root=libcxx,
            ):
                return [], {
                    "skipped_cxx": 0,
                    "skipped_static": 0,
                    "declarations": 0,
                    "target_inactive": core._target_inactive_record(
                        display=display,
                        sdk_root=root,
                        context_header=None,
                        reason=(
                            "unreferenced libc++ support leaf is not reachable from the "
                            "installed SDK include graph for the selected target"
                        ),
                    ),
                }
            raise context_error

    assert ast is not None
    if not target_entered:
        return [], {
            "skipped_cxx": 0,
            "skipped_static": 0,
            "declarations": 0,
            "target_inactive": core._target_inactive_record(
                display=display,
                sdk_root=root,
                context_header=context_header,
                reason=(
                    "SDK context did not enter this physical header for the selected target"
                ),
            ),
        }

    raw, skipped_cxx, skipped_static = _discover_raw_functions_for_source(
        ast,
        header=display,
        source_path=resolved,
    )
    if not raw:
        return [], {
            "skipped_cxx": skipped_cxx,
            "skipped_static": skipped_static,
            "declarations": 0,
        }

    # libc++ ships internal platform-support leaves for many targets in every
    # SDK. Some compile successfully in isolation even though the selected
    # target's owning libc++ header conditionally chooses a different platform
    # implementation. A support leaf may contribute C ABI declarations only
    # after an actual installed SDK owner compiles for this target and Clang's
    # dependency output proves that owner entered the leaf. This prevents a
    # standalone non-Darwin support implementation from overriding Darwin's
    # real declarations while still keeping the physical header in coverage.
    if _is_libcxx_support_header(resolved, root):
        libcxx = core._libcxx_root(root)
        assert libcxx is not None
        owner_candidates = sdk_header_context.reverse_context_candidates(
            resolved,
            sdk_root=root,
            libcxx_root=libcxx,
        )
        if not owner_candidates:
            return [], {
                "skipped_cxx": 0,
                "skipped_static": 0,
                "declarations": 0,
                "target_inactive": core._target_inactive_record(
                    display=display,
                    sdk_root=root,
                    context_header=None,
                    reason=(
                        "libc++ support leaf has no owner in the installed SDK include graph"
                    ),
                ),
            }

        selected_ast: dict | None = None
        selected_context: Path | None = None
        selected_language = language
        selected_extra_args: tuple[str, ...] = ()
        inactive_context: Path | None = None
        owner_errors: list[str] = []
        for candidate in owner_candidates:
            candidate_args = _context_extra_args(
                resolved,
                candidate,
                root,
                extra_args,
            )
            for candidate_language in _candidate_languages(
                language,
                candidate,
                target_header=resolved,
                sdk_root=root,
            ):
                try:
                    candidate_ast, candidate_entered = core._parse_context_translation_unit(
                        context_header=candidate,
                        target_header=resolved,
                        include_target=False,
                        display=display,
                        clang=clang,
                        target=target,
                        sdk_root=root,
                        language=candidate_language,
                        extra_args=candidate_args,
                        replacements=replacements,
                        timeout_seconds=timeout_seconds,
                    )
                except header_surface.HeaderParseError as exc:
                    owner_errors.append(str(exc))
                    continue
                if candidate_entered:
                    selected_ast = candidate_ast
                    selected_context = candidate
                    selected_language = candidate_language
                    selected_extra_args = candidate_args
                    break
                if inactive_context is None:
                    inactive_context = candidate
            if selected_ast is not None:
                break

        if selected_ast is None:
            if inactive_context is not None:
                return [], {
                    "skipped_cxx": 0,
                    "skipped_static": 0,
                    "declarations": 0,
                    "target_inactive": core._target_inactive_record(
                        display=display,
                        sdk_root=root,
                        context_header=inactive_context,
                        reason=(
                            "libc++ support leaf is present but no compiling SDK owner "
                            "activates it for the selected target"
                        ),
                    ),
                }
            detail = owner_errors[0] if owner_errors else "no owner context succeeded"
            raise header_surface.HeaderParseError(
                f"{display}: libc++ support leaf target reachability could not be proven: "
                f"{detail}"
            )

        assert selected_context is not None
        context_header = selected_context
        language = selected_language
        active_extra_args = selected_extra_args
        raw, skipped_cxx, skipped_static = _discover_raw_functions_for_source(
            selected_ast,
            header=display,
            source_path=resolved,
        )
        if not raw:
            return [], {
                "skipped_cxx": skipped_cxx,
                "skipped_static": skipped_static,
                "declarations": 0,
            }

    signatures, unavailable = core._recover_contextual_signatures(
        raw=raw,
        context_header=context_header,
        target_header=resolved,
        display=display,
        clang=clang,
        target=target,
        sdk_root=root,
        language=language,
        extra_args=active_extra_args,
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
    """Analyze physical headers concurrently with deterministic error ordering."""
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
            except Exception as exc:
                errors[index] = exc
            completed += 1
            if progress_stream is not None and (
                completed == len(ordered) or completed % progress_every == 0
            ):
                print(
                    core._progress_line(
                        completed=completed,
                        total=len(ordered),
                        started=started,
                        display=ordered[index][1],
                    ),
                    file=progress_stream,
                    flush=True,
                )

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
    target_inactive = []
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
        inactive_record = stats.get("target_inactive")
        if inactive_record is not None:
            target_inactive.append(inactive_record)

    manifest = header_surface.build_manifest(
        all_signatures,
        target=target,
        headers=[display for _, display in ordered],
        stats=all_stats,
    )
    if unavailable:
        rendered_unavailable = core._sorted_unavailable(unavailable)
        manifest["summary"]["target_unavailable_declaration_count"] = len(
            rendered_unavailable
        )
        manifest["target_unavailable"] = rendered_unavailable
    if target_inactive:
        rendered_inactive = core._sorted_target_inactive(target_inactive)
        manifest["summary"]["target_inactive_header_count"] = len(rendered_inactive)
        manifest["target_inactive_headers"] = rendered_inactive
    return manifest


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("headers", nargs="*", help="explicit header files")
    parser.add_argument("--sdk-root", help="SDK root used for includes and relative-path scanning")
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
    parser.add_argument("--timeout", type=int, default=120, help="per-Clang-invocation timeout in seconds")
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
    parser.add_argument("--output", help="write JSON manifest to this file instead of stdout")
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
            raise SdkHeaderSurfaceError(f"Clang executable not found: {args.clang}")
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
    except (SdkHeaderSurfaceError, header_surface.HeaderParseError, OSError) as exc:
        print(f"[sdk-header-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
