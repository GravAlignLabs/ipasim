#!/usr/bin/env python3
"""Diagnostic-driven SDK context recovery for AAPCS64 lowering.

The exhaustive header pass already proves that some legacy SDK declaration owners
need a prerequisite header that is not textually included by the leaf itself. The
ABI pass must preserve the same fail-closed rule: first compile the normal SDK-derived
wrappers, then use only the resulting Clang diagnostics plus SDK-owned header evidence
to add missing prerequisite context and retry the complete AAPCS64 surface.

Recovery never drops a symbol or treats a failed batch as usable evidence. Every
candidate comes from an explicit SDK include instruction or from a typedef/macro
provider found in the same SDK, and the complete compiler-backed pass must succeed
before a manifest is returned.
"""
from __future__ import annotations

import re
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import abi_surface
import compiler_batching
import sdk_abi_context
import sdk_header_context


MAX_RECOVERY_PASSES = 4


class SdkAbiRecoveryError(compiler_batching.CompilerBatchError):
    """Raised when diagnostic-driven SDK ABI context cannot be recovered safely."""


_SDK_ERROR_LINE = re.compile(
    r"^<SDKROOT>/(.+?\.h):\d+:\d+:\s+(?:fatal\s+)?error:\s+(.+)$",
    re.MULTILINE,
)
_DIRECT_INCLUDE_HINT = re.compile(
    r"\bdirectly\s*,\s*include\s+<?([A-Za-z0-9_./+\-]+\.h)>?",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class RecoveryCandidates:
    explicit: tuple[Path, ...]
    providers: tuple[Path, ...]


def _normalize_candidate(
    candidate: Path,
    *,
    source: Path,
    sdk_root: Path,
) -> Path | None:
    root = sdk_root.resolve()
    resolved = sdk_abi_context._prefer_usr_owner_for_non_libcxx(
        candidate,
        source=source,
        sdk_root=root,
    ).resolve()
    if resolved == source.resolve() or not resolved.is_file():
        return None
    try:
        resolved.relative_to(root)
    except ValueError:
        return None
    return resolved


def _dedupe_candidates(
    candidates: Sequence[Path],
    *,
    source: Path,
    sdk_root: Path,
) -> tuple[Path, ...]:
    result: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        normalized = _normalize_candidate(
            candidate,
            source=source,
            sdk_root=sdk_root,
        )
        if normalized is None:
            continue
        key = normalized.as_posix()
        if key in seen:
            continue
        seen.add(key)
        result.append(normalized)
    return tuple(result)


def diagnostic_recovery_candidates(
    diagnostic: str,
    *,
    header_root: Path,
    sdk_root: Path,
    source_headers: Sequence[str],
) -> dict[str, RecoveryCandidates]:
    """Derive per-source prerequisites solely from Clang + installed SDK evidence."""
    source_set = set(source_headers)
    grouped: dict[str, list[str]] = {}
    for match in _SDK_ERROR_LINE.finditer(diagnostic):
        relative = match.group(1).replace("\\", "/")
        if relative not in source_set:
            continue
        grouped.setdefault(relative, []).append(match.group(0))

    result: dict[str, RecoveryCandidates] = {}
    libcxx_root = sdk_abi_context._libcxx_root(sdk_root)
    for relative in sorted(grouped):
        source = sdk_abi_context._source_path(header_root, relative)
        message = "\n".join(grouped[relative])

        explicit: list[Path] = []
        explicit.extend(
            sdk_header_context.recommended_context_headers(
                message,
                target_header=source,
                sdk_root=sdk_root,
                libcxx_root=libcxx_root,
            )
        )

        # Some Darwin headers use the equivalent wording "do not include ...
        # directly, include X" rather than "include X instead". Normalize only
        # the named SDK header into the existing recommendation resolver.
        for spec in _DIRECT_INCLUDE_HINT.findall(message):
            explicit.extend(
                sdk_header_context.recommended_context_headers(
                    f"include {spec} instead of this file",
                    target_header=source,
                    sdk_root=sdk_root,
                    libcxx_root=libcxx_root,
                )
            )

        providers = sdk_header_context.definition_provider_candidates(
            message,
            target_header=source,
            sdk_root=sdk_root,
        )
        normalized_explicit = _dedupe_candidates(
            explicit,
            source=source,
            sdk_root=sdk_root,
        )
        normalized_providers = _dedupe_candidates(
            providers,
            source=source,
            sdk_root=sdk_root,
        )
        if normalized_explicit or normalized_providers:
            result[relative] = RecoveryCandidates(
                explicit=normalized_explicit,
                providers=normalized_providers,
            )
    return result


def _write_wrapper(
    wrapper_root: Path,
    *,
    relative: str,
    source: Path,
    sdk_root: Path,
    selected_c_names: Sequence[str],
    recovery_preludes: Sequence[Path],
) -> None:
    """Write the normal SDK ABI wrapper with proven recovery preludes prepended."""
    rel = Path(relative)
    if rel.is_absolute() or ".." in rel.parts:
        raise SdkAbiRecoveryError(
            f"source header must stay inside header root: {relative}"
        )
    wrapper = wrapper_root / rel
    wrapper.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "/* Generated locally for SDK ABI probing; not compatibility policy. */"
    ]
    scoped_defines = sdk_abi_context._scoped_sdk_defines(
        source,
        sdk_root=sdk_root,
    )
    markers: list[tuple[str, str]] = []
    for index, (name, value) in enumerate(scoped_defines):
        marker = f"__IPASIM_ABI_CONTEXT_DEFINED_{index}_{name}"
        lines.extend(
            [
                f"#ifndef {name}",
                f"#define {name} {value}",
                f"#define {marker} 1",
                "#endif",
            ]
        )
        markers.append((marker, name))

    normal_preludes = sdk_abi_context.recommended_preludes(
        source,
        sdk_root=sdk_root,
    )
    combined: list[Path] = []
    seen: set[str] = set()
    for candidate in (*recovery_preludes, *normal_preludes):
        resolved = candidate.resolve()
        key = resolved.as_posix()
        if resolved == source.resolve() or key in seen:
            continue
        seen.add(key)
        combined.append(resolved)

    reinclude_safe = sdk_abi_context._source_has_reinclude_guard(source)
    source_entered = False
    for context in combined:
        lines.append(f'#include "{sdk_abi_context._escaped_include(context)}"')
        if (
            not reinclude_safe
            and not source_entered
            and sdk_abi_context._prelude_reaches_source(
                context,
                source,
                sdk_root=sdk_root,
            )
        ):
            source_entered = True

    if reinclude_safe or not source_entered:
        lines.append(f'#include "{sdk_abi_context._escaped_include(source)}"')

    for marker, name in reversed(markers):
        lines.extend(
            [
                f"#ifdef {marker}",
                f"#undef {name}",
                f"#undef {marker}",
                "#endif",
            ]
        )

    for name in sorted(
        {sdk_abi_context._validate_c_name(item) for item in selected_c_names}
    ):
        lines.extend(
            [
                f"#ifdef {name}",
                f"#undef {name}",
                "#endif",
            ]
        )
    wrapper.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _run_with_recovery_preludes(
    inventory: dict,
    *,
    source_root: Path,
    sdk_root: Path,
    source_headers: Sequence[str],
    names_by_header: dict[str, set[str]],
    recovery_preludes: dict[str, list[Path]],
    batch_size: int,
    clang: str,
    extra_args: Sequence[str],
    timeout_seconds: int,
) -> dict:
    probe_extra_args = sdk_abi_context._aapcs64_extra_args(
        extra_args,
        sdk_root=sdk_root,
    )
    with tempfile.TemporaryDirectory(prefix="ipasim-sdk-abi-recovery-") as directory:
        wrapper_root = Path(directory) / "headers"
        for relative in source_headers:
            source = sdk_abi_context._source_path(source_root, relative)
            _write_wrapper(
                wrapper_root,
                relative=relative,
                source=source,
                sdk_root=sdk_root,
                selected_c_names=sorted(names_by_header.get(relative, ())),
                recovery_preludes=recovery_preludes.get(relative, ()),
            )
        return compiler_batching.build_aapcs64_manifest(
            inventory,
            header_root=wrapper_root,
            sdk_root=sdk_root,
            batch_size=batch_size,
            clang=clang,
            extra_args=probe_extra_args,
            timeout_seconds=timeout_seconds,
        )


def build_aapcs64_manifest(
    inventory: dict,
    *,
    header_root: Path,
    sdk_root: Path | None,
    batch_size: int = compiler_batching.DEFAULT_COMPILER_BATCH_SIZE,
    clang: str = "clang",
    extra_args: Sequence[str] = (),
    timeout_seconds: int = 120,
) -> dict:
    """Run AAPCS64 and retry only with context proven by its own diagnostics."""
    if sdk_root is None:
        return sdk_abi_context.build_aapcs64_manifest(
            inventory,
            header_root=header_root,
            sdk_root=None,
            batch_size=batch_size,
            clang=clang,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )

    source_root = header_root.resolve()
    root = sdk_root.resolve()
    if not source_root.is_dir():
        raise SdkAbiRecoveryError(f"header root does not exist: {source_root.name}")
    if not root.is_dir():
        raise SdkAbiRecoveryError(f"SDK root does not exist: {root.name}")

    _, selected = abi_surface._validate_inventory(inventory)
    source_headers = sorted({item.source_header for item in selected})
    if not source_headers:
        return sdk_abi_context.build_aapcs64_manifest(
            inventory,
            header_root=source_root,
            sdk_root=root,
            batch_size=batch_size,
            clang=clang,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )

    names_by_header: dict[str, set[str]] = {}
    for item in selected:
        names_by_header.setdefault(item.source_header, set()).add(
            sdk_abi_context._validate_c_name(item.c_name)
        )

    try:
        return sdk_abi_context.build_aapcs64_manifest(
            inventory,
            header_root=source_root,
            sdk_root=root,
            batch_size=batch_size,
            clang=clang,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )
    except compiler_batching.CompilerBatchError as exc:
        failure = exc

    recovery_preludes: dict[str, list[Path]] = {}
    for _ in range(MAX_RECOVERY_PASSES):
        candidates = diagnostic_recovery_candidates(
            str(failure),
            header_root=source_root,
            sdk_root=root,
            source_headers=source_headers,
        )
        progressed = False
        for relative in sorted(candidates):
            existing = recovery_preludes.setdefault(relative, [])
            existing_keys = {path.resolve().as_posix() for path in existing}
            item = candidates[relative]

            for candidate in item.explicit:
                key = candidate.resolve().as_posix()
                if key not in existing_keys:
                    existing.append(candidate)
                    existing_keys.add(key)
                    progressed = True

            # Definition-provider ranking is heuristic evidence until Clang proves
            # it. Add one new provider per pass so a wrong provider cannot be
            # silently combined with later candidates into apparent success.
            for candidate in item.providers:
                key = candidate.resolve().as_posix()
                if key in existing_keys:
                    continue
                existing.append(candidate)
                progressed = True
                break

        if not progressed:
            raise failure

        try:
            return _run_with_recovery_preludes(
                inventory,
                source_root=source_root,
                sdk_root=root,
                source_headers=source_headers,
                names_by_header=names_by_header,
                recovery_preludes=recovery_preludes,
                batch_size=batch_size,
                clang=clang,
                extra_args=extra_args,
                timeout_seconds=timeout_seconds,
            )
        except compiler_batching.CompilerBatchError as exc:
            failure = exc

    raise failure
