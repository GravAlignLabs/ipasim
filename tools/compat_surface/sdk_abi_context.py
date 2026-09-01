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
explicitly recommended by a leaf, conventional SDK package/framework umbrellas when
the SDK's own include/import graph proves they reach the declaration owner, and
source-local Swift importer definitions already justified by SDK header context.
Guarded declaration headers are always included directly after their preludes so a
conditional umbrella edge cannot hide a declaration from the active target. Only
unguarded headers are suppressed when a proven prelude already enters them, because
re-entering those implementation leaves can redeclare enums/functions. It also
removes any source-level macro alias that shadows a selected exported C identifier
after the declaration owner has been entered, so the address probe binds to the
TAPI-backed declaration rather than an inline convenience implementation. No
application names, semantic-provider approvals, or header-specific exceptions are
introduced here.
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
_PRAGMA_ONCE = re.compile(
    r"^[ \t]*#[ \t]*pragma[ \t]+once\b",
    re.MULTILINE,
)
_IFNDEF_GUARD = re.compile(
    r"^[ \t]*#[ \t]*ifndef[ \t]+([A-Za-z_][A-Za-z0-9_]*)\b",
    re.MULTILINE,
)
_IF_NOT_DEFINED_GUARD = re.compile(
    r"^[ \t]*#[ \t]*if[ \t]+![ \t]*defined[ \t]*(?:\([ \t]*)?"
    r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*\)?",
    re.MULTILINE,
)


def _libcxx_root(sdk_root: Path) -> Path | None:
    candidate = sdk_root.resolve() / "usr" / "include" / "c++" / "v1"
    return candidate if candidate.is_dir() else None


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _escaped_include(path: Path) -> str:
    return str(path.resolve()).replace("\\", "/").replace('"', '\\"')


def _source_path(header_root: Path, relative: str) -> Path:
    try:
        return abi_surface._resolve_header(header_root.resolve(), relative)
    except abi_surface.AbiSurfaceError as exc:
        raise SdkAbiContextError(str(exc)) from exc


def _source_has_reinclude_guard(source: Path) -> bool:
    """Return whether SDK text proves that entering ``source`` twice is idempotent."""
    try:
        text = source.resolve().read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise SdkAbiContextError(
            f"could not read SDK source header {source.name}: {exc}"
        ) from exc
    if _PRAGMA_ONCE.search(text):
        return True

    prefix = "\n".join(text.splitlines()[:96])
    matches: list[tuple[int, str]] = []
    for pattern in (_IFNDEF_GUARD, _IF_NOT_DEFINED_GUARD):
        match = pattern.search(prefix)
        if match is not None:
            matches.append((match.start(), match.group(1)))
    if not matches:
        return False
    _, name = min(matches, key=lambda item: item[0])
    define = re.compile(
        rf"^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}(?:\b|[ \t])",
        re.MULTILINE,
    )
    return define.search(prefix) is not None


def _framework_layout(
    source: Path,
    *,
    sdk_root: Path,
) -> tuple[Path, str] | None:
    """Return the innermost framework Headers root and framework name."""
    frameworks_root = sdk_root.resolve() / "System" / "Library" / "Frameworks"
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


def _framework_header_reaches(
    entry: Path,
    target: Path,
    *,
    headers_root: Path,
    framework_name: str,
) -> bool:
    """Prove reachability using only include/import edges in one framework."""
    entry = entry.resolve()
    target = target.resolve()
    try:
        entry.relative_to(headers_root)
        target.relative_to(headers_root)
    except ValueError:
        return False
    if entry == target:
        return True

    queue = [entry]
    seen: set[str] = set()
    while queue:
        owner = queue.pop(0).resolve()
        key = str(owner)
        if key in seen:
            continue
        seen.add(key)
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
            if child == target:
                return True
            if str(child) not in seen:
                children.append(child)
        queue.extend(sorted(set(children), key=lambda path: path.as_posix()))
    return False


def _framework_umbrella(source: Path, *, sdk_root: Path) -> Path | None:
    """Return a canonical framework umbrella that reaches ``source``."""
    resolved = source.resolve()
    layout = _framework_layout(resolved, sdk_root=sdk_root)
    if layout is None:
        return None
    headers_root, framework_name = layout
    umbrella = (headers_root / f"{framework_name}.h").resolve()
    if not umbrella.is_file() or umbrella == resolved:
        return None
    if _framework_header_reaches(
        umbrella,
        resolved,
        headers_root=headers_root,
        framework_name=framework_name,
    ):
        return umbrella
    return None


def _resolve_usr_include(
    *,
    owner: Path,
    opener: str,
    spec: str,
    sdk_root: Path,
) -> Path | None:
    """Resolve an SDK usr/include edge without escaping that installed tree."""
    root = sdk_root.resolve()
    usr_include = (root / "usr" / "include").resolve()
    libcxx_root = _libcxx_root(root)
    candidates: list[Path] = []
    if opener == '"':
        candidates.append(owner.resolve().parent / spec)
    if libcxx_root is not None:
        candidates.append(libcxx_root / spec)
    candidates.append(usr_include / spec)

    seen: set[str] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        key = str(resolved)
        if key in seen:
            continue
        seen.add(key)
        try:
            resolved.relative_to(usr_include)
        except ValueError:
            continue
        if resolved.is_file():
            return resolved
    return None


def _usr_header_reaches(entry: Path, target: Path, *, sdk_root: Path) -> bool:
    """Prove reachability using only installed usr/include include/import edges."""
    root = sdk_root.resolve()
    usr_include = (root / "usr" / "include").resolve()
    entry = entry.resolve()
    target = target.resolve()
    try:
        entry.relative_to(usr_include)
        target.relative_to(usr_include)
    except ValueError:
        return False
    if entry == target:
        return True

    queue = [entry]
    seen: set[str] = set()
    while queue:
        owner = queue.pop(0).resolve()
        key = str(owner)
        if key in seen:
            continue
        seen.add(key)
        try:
            text = owner.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise SdkAbiContextError(
                f"could not read SDK include header {owner.name}: {exc}"
            ) from exc
        children: list[Path] = []
        for match in _INCLUDE_DIRECTIVE.finditer(text):
            child = _resolve_usr_include(
                owner=owner,
                opener=match.group(1),
                spec=match.group(2).strip(),
                sdk_root=root,
            )
            if child is None:
                continue
            if child == target:
                return True
            if str(child) not in seen:
                children.append(child)
        queue.extend(sorted(set(children), key=lambda path: path.as_posix()))
    return False


def _usr_package_umbrella(source: Path, *, sdk_root: Path) -> Path | None:
    """Return ``usr/include/<pkg>/<pkg>.h`` only when it reaches ``source``."""
    root = sdk_root.resolve()
    usr_include = (root / "usr" / "include").resolve()
    resolved = source.resolve()
    try:
        relative = resolved.relative_to(usr_include)
    except ValueError:
        return None
    if len(relative.parts) < 2 or relative.parts[0] == "c++":
        return None

    package = relative.parts[0]
    umbrella = (usr_include / package / f"{package}.h").resolve()
    if not umbrella.is_file() or umbrella == resolved:
        return None
    if _usr_header_reaches(umbrella, resolved, sdk_root=root):
        return umbrella
    return None


def _prefer_usr_owner_for_non_libcxx(
    candidate: Path,
    *,
    source: Path,
    sdk_root: Path,
) -> Path:
    """Prefer the ordinary SDK C header when a non-libc++ leaf names one."""
    root = sdk_root.resolve()
    libcxx = _libcxx_root(root)
    if libcxx is None:
        return candidate.resolve()
    resolved_source = source.resolve()
    resolved_candidate = candidate.resolve()
    if _is_within(resolved_source, libcxx):
        return resolved_candidate
    try:
        relative = resolved_candidate.relative_to(libcxx.resolve())
    except ValueError:
        return resolved_candidate
    ordinary = (root / "usr" / "include" / relative).resolve()
    if ordinary.is_file():
        return ordinary
    return resolved_candidate


def _prelude_reaches_source(
    prelude: Path,
    source: Path,
    *,
    sdk_root: Path,
) -> bool:
    """Return whether SDK include evidence proves ``prelude`` enters ``source``."""
    prelude = prelude.resolve()
    source = source.resolve()
    if prelude == source:
        return True

    layout = _framework_layout(source, sdk_root=sdk_root)
    if layout is not None:
        headers_root, framework_name = layout
        try:
            prelude.relative_to(headers_root)
        except ValueError:
            pass
        else:
            return _framework_header_reaches(
                prelude,
                source,
                headers_root=headers_root,
                framework_name=framework_name,
            )

    return _usr_header_reaches(prelude, source, sdk_root=sdk_root)


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
    candidates = [
        _prefer_usr_owner_for_non_libcxx(
            candidate,
            source=resolved,
            sdk_root=root,
        )
        for candidate in candidates
    ]
    package_umbrella = _usr_package_umbrella(resolved, sdk_root=root)
    if package_umbrella is not None:
        candidates.append(package_umbrella)
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


def _scoped_sdk_defines(
    source: Path,
    *,
    sdk_root: Path,
) -> list[tuple[str, str]]:
    """Return wrapper-scoped defines already justified by SDK importer context."""
    result: list[tuple[str, str]] = []
    for argument in sdk_header_context.swift_importer_args(source, sdk_root):
        if not argument.startswith("-D") or len(argument) <= 2:
            raise SdkAbiContextError(
                f"unsupported SDK importer argument for ABI wrapper: {argument!r}"
            )
        payload = argument[2:]
        name, separator, value = payload.partition("=")
        if not _C_IDENTIFIER.fullmatch(name):
            raise SdkAbiContextError(
                f"SDK importer define has unsafe identifier: {argument!r}"
            )
        if not separator:
            value = "1"
        if not value or "\n" in value or "\r" in value:
            raise SdkAbiContextError(
                f"SDK importer define has unsafe value: {argument!r}"
            )
        result.append((name, value))
    return result


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
    scoped_defines = _scoped_sdk_defines(source, sdk_root=sdk_root)
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

    preludes = recommended_preludes(source, sdk_root=sdk_root)
    reinclude_safe = _source_has_reinclude_guard(source)
    source_entered = False
    for context in preludes:
        lines.append(f'#include "{_escaped_include(context)}"')
        if (
            not reinclude_safe
            and not source_entered
            and _prelude_reaches_source(
                context,
                source,
                sdk_root=sdk_root,
            )
        ):
            source_entered = True

    if reinclude_safe or not source_entered:
        lines.append(f'#include "{_escaped_include(source)}"')

    for marker, name in reversed(markers):
        lines.extend(
            [
                f"#ifdef {marker}",
                f"#undef {name}",
                f"#undef {marker}",
                "#endif",
            ]
        )

    for name in sorted({_validate_c_name(item) for item in selected_c_names}):
        lines.extend(
            [
                f"#ifdef {name}",
                f"#undef {name}",
                "#endif",
            ]
        )
    wrapper.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _aapcs64_extra_args(
    extra_args: Sequence[str],
    *,
    sdk_root: Path,
) -> tuple[str, ...]:
    """Add SDK include roots needed by compiler-backed ABI wrapper translation."""
    result = list(extra_args)
    libcxx = _libcxx_root(sdk_root)
    if libcxx is not None:
        path = str(libcxx.resolve())
        if path not in result:
            result.extend(["-I", path])
    return tuple(result)


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

    probe_extra_args = _aapcs64_extra_args(extra_args, sdk_root=root)
    _, selected = abi_surface._validate_inventory(inventory)
    source_headers = sorted({item.source_header for item in selected})
    if not source_headers:
        return compiler_batching.build_aapcs64_manifest(
            inventory,
            header_root=source_root,
            sdk_root=root,
            batch_size=batch_size,
            clang=clang,
            extra_args=probe_extra_args,
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
                extra_args=probe_extra_args,
                timeout_seconds=timeout_seconds,
            )
        except abi_surface.AbiSurfaceError as exc:
            raise compiler_batching.CompilerBatchError(str(exc)) from exc
