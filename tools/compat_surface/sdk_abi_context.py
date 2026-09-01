#!/usr/bin/env python3
"""SDK-derived include context for compiler-backed ABI lowering.

Header indexing deliberately attributes each declaration to the physical SDK header
that owns it. Some Apple SDK leaf headers are not legal translation-unit entrypoints,
however; the SDK explicitly requires clients to include a public umbrella/prelude
first. ABI lowering must preserve that distinction instead of directly including
an origin leaf and losing the context Clang already proved during header indexing.

This module builds a temporary header-root of tiny wrappers. Each wrapper keeps the
original physical source path stable for the existing ABI machinery, but prepends
only public/prelude headers explicitly recommended by the SDK's own header text.
No application names, semantic-provider approvals, or header-specific exceptions
are introduced here.
"""
from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Sequence

import abi_surface
import compiler_batching
import sdk_header_context


class SdkAbiContextError(ValueError):
    """Raised when SDK-backed ABI include context cannot be constructed safely."""


def _libcxx_root(sdk_root: Path) -> Path | None:
    candidate = sdk_root.resolve() / "usr" / "include" / "c++" / "v1"
    return candidate if candidate.is_dir() else None


def _escaped_include(path: Path) -> str:
    return str(path.resolve()).replace("\\", "/").replace('"', '\\"')


def _source_path(header_root: Path, relative: str) -> Path:
    try:
        return abi_surface._resolve_header(header_root.resolve(), relative)
    except abi_surface.AbiSurfaceError as exc:
        raise SdkAbiContextError(str(exc)) from exc


def recommended_preludes(
    source: Path,
    *,
    sdk_root: Path,
) -> list[Path]:
    """Return SDK headers the physical source explicitly says to include first."""
    root = sdk_root.resolve()
    resolved = source.resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        return []
    try:
        text = resolved.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise SdkAbiContextError(
            f"could not read SDK source header {resolved.name}: {exc}"
        ) from exc
    return sdk_header_context.recommended_context_headers(
        text,
        target_header=resolved,
        sdk_root=root,
        libcxx_root=_libcxx_root(root),
    )


def _write_wrapper(
    wrapper_root: Path,
    *,
    relative: str,
    source: Path,
    sdk_root: Path,
) -> None:
    rel = Path(relative)
    if rel.is_absolute() or ".." in rel.parts:
        raise SdkAbiContextError(
            f"source header must stay inside header root: {relative}"
        )
    wrapper = wrapper_root / rel
    wrapper.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "/* Generated locally for SDK ABI probing; not compatibility policy. */"
    ]
    for context in recommended_preludes(source, sdk_root=sdk_root):
        lines.append(f'#include "{_escaped_include(context)}"')
    # Always include the physical declaration owner after its SDK-authored
    # prelude. If the public owner already entered the leaf, its normal include
    # guard makes this a no-op; otherwise this preserves prerequisite semantics.
    lines.append(f'#include "{_escaped_include(source)}"')
    wrapper.write_text("\n".join(lines) + "\n", encoding="utf-8")


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
    """Lower AAPCS64 using SDK-authored public include context when required."""
    if sdk_root is None:
        return compiler_batching.build_aapcs64_manifest(
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
        raise SdkAbiContextError(f"header root does not exist: {source_root.name}")
    if not root.is_dir():
        raise SdkAbiContextError(f"SDK root does not exist: {root.name}")

    _, selected = abi_surface._validate_inventory(inventory)
    source_headers = sorted({item.source_header for item in selected})
    if not source_headers:
        return compiler_batching.build_aapcs64_manifest(
            inventory,
            header_root=source_root,
            sdk_root=root,
            batch_size=batch_size,
            clang=clang,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )

    with tempfile.TemporaryDirectory(prefix="ipasim-sdk-abi-context-") as directory:
        wrapper_root = Path(directory) / "headers"
        for relative in source_headers:
            source = _source_path(source_root, relative)
            _write_wrapper(
                wrapper_root,
                relative=relative,
                source=source,
                sdk_root=root,
            )
        return compiler_batching.build_aapcs64_manifest(
            inventory,
            header_root=wrapper_root,
            sdk_root=root,
            batch_size=batch_size,
            clang=clang,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )
