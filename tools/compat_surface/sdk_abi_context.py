#!/usr/bin/env python3
"""SDK-derived include context for compiler-backed ABI lowering.

Header indexing deliberately attributes each declaration to the physical SDK header
that owns it. Some Apple SDK leaf headers are not legal translation-unit entrypoints,
however; the SDK explicitly requires clients to include a public umbrella/prelude
first. ABI lowering must preserve that distinction instead of directly including
an origin leaf and losing the context Clang already proved during header indexing.

This module builds a temporary header-root of tiny wrappers. Each wrapper keeps the
original physical source path stable for the existing ABI machinery, but prepends
only public/prelude headers derived from the SDK itself. That includes headers
explicitly recommended by a leaf and a framework's canonical umbrella when the
SDK's own include/import graph proves that umbrella reaches the declaration owner.
It also removes any source-level macro alias that shadows a selected exported C
identifier after the declaration owner has been entered, so the address probe binds
to the TAPI-backed declaration rather than an inline convenience implementation.
No application names, semantic-provider approvals, or header-specific exceptions
are introduced here.
"""
from __future__ import annotations

import re
import tempfile
from pathlib import Path
from typing import Sequence

import abi_surface
import compiler_batching
import sdk_header_context


class SdkAbiContextError(compiler_batching.CompilerBatchError):
    """Raised when SDK-backed ABI include context cannot be constructed safely."""


_C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_INCLUDE_DIRECTIVE = re.compile(
    r'^[ \t]*#[ \t]*(?:include|import)[ \t]*([<"])([^>"]+)[>"]',
    re.MULTILINE,
)


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


def _framework_layout(
    source: Path,
    *,
    sdk_root: Path,
) -> tuple[Path, str] | None:
    """Return the innermost framework Headers root and framework name."""
    frameworks_root = (
        sdk_root.resolve() / "System" / "Library" / "Frameworks"
    )
    try:
        relative = source.resolve().relative_to(frameworks_root)
    except ValueError:
        return None

    parts = relative.parts
    candidates = [
        index
        for index, part in enumerate(parts[:-1])
        if part.endswith(".framework")
        and index + 1 < len(parts)
        and parts[index + 1] == "Headers"
    ]
    if not candidates:
        return None
    index = candidates[-1]
    framework_dir = frameworks_root.joinpath(*parts[: index + 1])
    headers_root = framework_dir / "Headers"
    if not headers_root.is_dir():
        return None
    framework_name = parts[index][: -len(".framework")]
    return headers_root.resolve(), framework_name


def _resolve_framework_include(
    *,
    owner: Path,
    opener: str,
    spec: str,
    headers_root: Path,
    framework_name: str,
) -> Path | None:
    """Resolve one include only when it stays in the same framework Headers tree."""
    candidates: list[Path] = []
    if opener == '"':
        candidates.append(owner.resolve().parent / spec)

    prefix = framework_name + "/"
    if spec.startswith(prefix):
        candidates.append(headers_root / spec[len(prefix) :])
    else:
        candidates.append(headers_root / spec)

    seen: set[str] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        key = str(resolved)
        if key in seen:
            continue
        seen.add(key)
        try:
            resolved.relative_to(headers_root)
        except ValueError:
            continue
        if resolved.is_file():
            return resolved
    return None


def _framework_umbrella(source: Path, *, sdk_root: Path) -> Path | None:
    """Return a canonical framework umbrella that reaches ``source``.

    Apple SDK frameworks frequently put implementation leaves one or more include
    levels below their public master header. The leaf may simply say "do not include
    directly" without naming that owner. We accept the conventional
    ``Headers/<Framework>.h`` entrypoint only when a deterministic traversal of the
    installed SDK's own include/import directives reaches the exact physical source.
    This supports nested frameworks as well as direct umbrella-to-leaf relationships
    without inventing prerequisites or framework-specific policy.
    """
    resolved = source.resolve()
    layout = _framework_layout(resolved, sdk_root=sdk_root)
    if layout is None:
        return None
    headers_root, framework_name = layout
    umbrella = (headers_root / f"{framework_name}.h").resolve()
    if not umbrella.is_file() or umbrella == resolved:
        return None

    queue = [umbrella]
    seen: set[str] = set()
    while queue:
        owner = queue.pop(0).resolve()
        key = str(owner)
        if key in seen:
            continue
        seen.add(key)
        if owner == resolved:
            return umbrella
        try:
            text = owner.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise SdkAbiContextError(
                f"could not read SDK framework header {owner.name}: {exc}"
            ) from exc
        children: list[Path] = []
        for match in _INCLUDE_DIRECTIVE.finditer(text):
            child = _resolve_framework_include(
                owner=owner,
                opener=match.group(1),
                spec=match.group(2).strip(),
                headers_root=headers_root,
                framework_name=framework_name,
            )
            if child is None:
                continue
            if child == resolved:
                return umbrella
            if str(child) not in seen:
                children.append(child)
        queue.extend(sorted(set(children), key=lambda path: path.as_posix()))
    return None


def recommended_preludes(
    source: Path,
    *,
    sdk_root: Path,
) -> list[Path]:
    """Return SDK-proven headers that must precede the physical declaration owner."""
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

    candidates = sdk_header_context.recommended_context_headers(
        text,
        target_header=resolved,
        sdk_root=root,
        libcxx_root=_libcxx_root(root),
    )
    framework_umbrella = _framework_umbrella(resolved, sdk_root=root)
    if framework_umbrella is not None:
        candidates.append(framework_umbrella)

    result: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        candidate = candidate.resolve()
        key = str(candidate)
        if candidate == resolved or key in seen:
            continue
        seen.add(key)
        result.append(candidate)
    return result


def _validate_c_name(name: str) -> str:
    if not isinstance(name, str) or not _C_IDENTIFIER.fullmatch(name):
        raise SdkAbiContextError(
            f"selected SDK C identifier cannot be represented safely: {name!r}"
        )
    return name


def _write_wrapper(
    wrapper_root: Path,
    *,
    relative: str,
    source: Path,
    sdk_root: Path,
    selected_c_names: Sequence[str] = (),
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
    # guard/pragma-once handling makes this a no-op; otherwise this preserves
    # prerequisite semantics.
    lines.append(f'#include "{_escaped_include(source)}"')

    # Some public SDK headers declare a real exported function and then replace
    # that spelling with an object-like macro to a static inline implementation.
    # The ABI inventory is keyed to the TAPI export and the original FunctionDecl,
    # so allowing that later macro to rewrite ``&name`` would make Clang lower the
    # convenience helper instead of the exported symbol. Undefine only C names
    # selected from this physical declaration owner, after the owner has compiled.
    # Header guards ensure later indirect includes cannot replay this owner's macro
    # definitions in the same ABI probe translation unit.
    for name in sorted({_validate_c_name(item) for item in selected_c_names}):
        lines.extend(
            [
                f"#ifdef {name}",
                f"#undef {name}",
                "#endif",
            ]
        )
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

    names_by_header: dict[str, set[str]] = {}
    for item in selected:
        names_by_header.setdefault(item.source_header, set()).add(
            _validate_c_name(item.c_name)
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
                selected_c_names=sorted(names_by_header.get(relative, ())),
            )
        try:
            return compiler_batching.build_aapcs64_manifest(
                inventory,
                header_root=wrapper_root,
                sdk_root=root,
                batch_size=batch_size,
                clang=clang,
                extra_args=extra_args,
                timeout_seconds=timeout_seconds,
            )
        except abi_surface.AbiSurfaceError as exc:
            raise compiler_batching.CompilerBatchError(str(exc)) from exc
