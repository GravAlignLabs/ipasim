#!/usr/bin/env python3
"""SDK header ownership/reachability helpers for exhaustive Clang analysis.

This module is deliberately mechanical. It derives contextual ownership from
header text that already exists in the SDK: explicit include recommendations,
source language/module requirements, definition providers, and the SDK's own
include/import graph. It never approves runtime semantics and never removes a
physical header from exhaustive coverage.
"""
from __future__ import annotations

import functools
import os
import re
from pathlib import Path

_INCLUDE_DIRECTIVE = re.compile(
    r'^[ \t]*#[ \t]*(?:include|import)[ \t]*([<"])([^>"]+)[>"]',
    re.MULTILINE,
)
_RECOMMENDED_ANGLE = re.compile(
    r'(?:include|use)\s+<([^>]+)>\s+instead',
    re.IGNORECASE,
)
_RECOMMENDED_BARE = re.compile(
    r'\binclude\s+([A-Za-z0-9_./+\-]+\.h)\s+instead(?:\s+of\s+this\s+file)?',
    re.IGNORECASE,
)
_CXX_MODELINE = re.compile(
    r'-\*-\s*(?:mode\s*:\s*)?(?:c\+\+|objective-c\+\+)\s*-\*-',
    re.IGNORECASE,
)
_MODULE_IMPORT = re.compile(
    r'^[ \t]*@import\s+[A-Za-z_][A-Za-z0-9_.]*\s*;',
    re.MULTILINE,
)
_MODULE_FEATURE = re.compile(r'__has_feature\s*\(\s*modules\s*\)')
_PP_ERROR = re.compile(r'^[ \t]*#[ \t]*error[ \t]+(.+)$', re.MULTILINE)
_UNKNOWN_TYPE = re.compile(r"error:\s+unknown type name '([^']+)'", re.IGNORECASE)
_MISSING_HEADER = re.compile(
    r"fatal error:\s+['<]([^'>]+)[>']\s+file not found",
    re.IGNORECASE,
)
_DEFINE_NAME = re.compile(
    r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)\b',
    re.MULTILINE,
)
# Deliberately conservative: this catches ordinary typedef declarations. False
# positives only add a candidate context; Clang still has to prove it works.
_TYPEDEF_NAME = re.compile(
    r'\btypedef\b(?:(?!;).){0,512}?\b([A-Za-z_][A-Za-z0-9_]*)\s*;',
    re.DOTALL,
)


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _key(path: Path) -> str:
    return os.path.normcase(os.path.abspath(str(path.resolve())))


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def relative_sdk_path(path: Path, sdk_root: Path) -> str:
    return path.resolve().relative_to(sdk_root.resolve()).as_posix()


def preferred_clang_language(path: Path) -> str:
    """Derive whether a physical header explicitly asks to be parsed as C++."""
    text = _read_text(path)
    first_lines = "\n".join(text.splitlines()[:8])
    if _CXX_MODELINE.search(first_lines):
        return "objective-c++"
    return "objective-c"


def requires_clang_modules(path: Path) -> bool:
    """Return whether the header itself proves that Clang modules are required."""
    text = _read_text(path)
    if _MODULE_IMPORT.search(text):
        return True
    # Private/module-only SDK leaves commonly guard direct textual inclusion by
    # checking __has_feature(modules) and raising #error otherwise.
    return bool(_MODULE_FEATURE.search(text) and _PP_ERROR.search(text))


def _resolve_include(
    *,
    source: Path,
    spec: str,
    quoted: bool,
    sdk_root: Path,
    libcxx_root: Path | None,
) -> Path | None:
    root = sdk_root.resolve()
    usr_include = root / "usr" / "include"
    candidates: list[Path] = []
    if quoted:
        candidates.append(source.resolve().parent / spec)
    if libcxx_root is not None:
        candidates.append(libcxx_root.resolve() / spec)
    candidates.append(usr_include / spec)

    seen: set[str] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        key = _key(resolved)
        if key in seen:
            continue
        seen.add(key)
        if not resolved.is_file():
            continue
        if not _is_within(resolved, usr_include):
            continue
        return resolved
    return None


@functools.lru_cache(maxsize=8)
def _reverse_include_index_cached(
    sdk_root_text: str,
    libcxx_root_text: str,
) -> dict[str, tuple[Path, ...]]:
    sdk_root = Path(sdk_root_text).resolve()
    libcxx_root = Path(libcxx_root_text).resolve() if libcxx_root_text else None
    usr_include = sdk_root / "usr" / "include"
    if not usr_include.is_dir():
        return {}

    owners: dict[str, set[Path]] = {}
    for source in sorted(
        (path for path in usr_include.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(usr_include).as_posix(),
    ):
        text = _read_text(source)
        for match in _INCLUDE_DIRECTIVE.finditer(text):
            opener = match.group(1)
            spec = match.group(2).strip()
            if not spec:
                continue
            target = _resolve_include(
                source=source,
                spec=spec,
                quoted=opener == '"',
                sdk_root=sdk_root,
                libcxx_root=libcxx_root,
            )
            if target is None:
                continue
            owners.setdefault(_key(target), set()).add(source.resolve())

    return {
        key: tuple(sorted(values, key=lambda path: str(path)))
        for key, values in owners.items()
    }


def _reverse_include_index(
    sdk_root: Path,
    libcxx_root: Path | None,
) -> dict[str, tuple[Path, ...]]:
    return _reverse_include_index_cached(
        str(sdk_root.resolve()),
        str(libcxx_root.resolve()) if libcxx_root is not None else "",
    )


@functools.lru_cache(maxsize=8)
def _definition_provider_index_cached(
    sdk_root_text: str,
) -> dict[str, tuple[Path, ...]]:
    """Index explicit typedef/macro providers in SDK usr/include once per SDK."""
    sdk_root = Path(sdk_root_text).resolve()
    usr_include = sdk_root / "usr" / "include"
    if not usr_include.is_dir():
        return {}

    providers: dict[str, set[Path]] = {}
    for source in sorted(
        (path for path in usr_include.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(usr_include).as_posix(),
    ):
        text = _read_text(source)
        if not text:
            continue
        names = set(_DEFINE_NAME.findall(text))
        names.update(_TYPEDEF_NAME.findall(text))
        for name in names:
            providers.setdefault(name, set()).add(source.resolve())
    return {
        name: tuple(sorted(paths, key=lambda path: str(path)))
        for name, paths in providers.items()
    }


def _definition_provider_index(sdk_root: Path) -> dict[str, tuple[Path, ...]]:
    return _definition_provider_index_cached(str(sdk_root.resolve()))


def _owner_score(path: Path, sdk_root: Path) -> tuple[int, int, int, str]:
    usr_include = sdk_root.resolve() / "usr" / "include"
    try:
        relative = path.resolve().relative_to(usr_include)
    except ValueError:
        relative = path.resolve()
    parts = relative.parts
    private_parts = sum(1 for part in parts if part.startswith("_"))
    support_parts = sum(1 for part in parts if part == "__support")
    return private_parts, support_parts, len(parts), relative.as_posix()


def reverse_context_candidates(
    path: Path,
    *,
    sdk_root: Path,
    libcxx_root: Path | None,
    max_depth: int = 5,
    max_candidates: int = 64,
) -> list[Path]:
    """Return deterministic transitive textual includers for one SDK header."""
    resolved = path.resolve()
    index = _reverse_include_index(sdk_root, libcxx_root)
    queue: list[tuple[Path, int]] = [(resolved, 0)]
    seen = {_key(resolved)}
    found: list[tuple[int, Path]] = []

    cursor = 0
    while cursor < len(queue) and len(found) < max_candidates:
        current, depth = queue[cursor]
        cursor += 1
        if depth >= max_depth:
            continue
        owners = sorted(
            index.get(_key(current), ()),
            key=lambda item: _owner_score(item, sdk_root),
        )
        for owner in owners:
            owner_key = _key(owner)
            if owner_key in seen:
                continue
            seen.add(owner_key)
            found.append((depth + 1, owner))
            queue.append((owner, depth + 1))
            if len(found) >= max_candidates:
                break

    found.sort(
        key=lambda item: (
            item[0],
            *_owner_score(item[1], sdk_root),
        )
    )
    return [path for _, path in found]


def definition_provider_candidates(
    message: str,
    *,
    target_header: Path,
    sdk_root: Path,
    max_candidates: int = 24,
) -> list[Path]:
    """Derive prerequisite headers from identifiers Clang proves are undefined.

    This is a recovery path for legacy C headers that are physically present in
    the SDK but assume a public prelude was included first. Candidate headers
    come only from typedef/#define evidence in the same SDK, and every candidate
    still has to compile successfully with the target before it is accepted.
    """
    names = sorted(set(_UNKNOWN_TYPE.findall(message)))
    if not names:
        return []
    index = _definition_provider_index(sdk_root)
    target_key = _key(target_header)
    coverage: dict[Path, set[str]] = {}
    for name in names:
        for provider in index.get(name, ()):
            if _key(provider) == target_key:
                continue
            coverage.setdefault(provider, set()).add(name)

    ranked = sorted(
        coverage,
        key=lambda path: (
            -len(coverage[path]),
            *_owner_score(path, sdk_root),
        ),
    )
    return ranked[:max_candidates]


def _resolve_recommended_spec(
    spec: str,
    *,
    target_header: Path,
    sdk_root: Path,
    libcxx_root: Path | None,
    prefer_sibling: bool,
) -> Path | None:
    root = sdk_root.resolve()
    usr_include = root / "usr" / "include"
    candidates: list[Path] = []
    if prefer_sibling:
        candidates.append(target_header.resolve().parent / spec)
    if libcxx_root is not None:
        candidates.append(libcxx_root.resolve() / spec)
    candidates.append(usr_include / spec)
    if not prefer_sibling:
        candidates.append(target_header.resolve().parent / spec)

    seen: set[str] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        key = _key(resolved)
        if key in seen:
            continue
        seen.add(key)
        if resolved.is_file() and _is_within(resolved, usr_include):
            return resolved
    return None


def recommended_context_headers(
    message: str,
    *,
    target_header: Path,
    sdk_root: Path,
    libcxx_root: Path | None,
) -> list[Path]:
    """Resolve public/prelude headers explicitly named by SDK diagnostics."""
    specs: list[tuple[str, bool]] = []
    specs.extend((match, False) for match in _RECOMMENDED_ANGLE.findall(message))
    specs.extend((match, True) for match in _RECOMMENDED_BARE.findall(message))

    result: list[Path] = []
    seen: set[str] = set()
    for spec, prefer_sibling in specs:
        resolved = _resolve_recommended_spec(
            spec.strip(),
            target_header=target_header,
            sdk_root=sdk_root,
            libcxx_root=libcxx_root,
            prefer_sibling=prefer_sibling,
        )
        if resolved is None or resolved == target_header.resolve():
            continue
        key = _key(resolved)
        if key in seen:
            continue
        seen.add(key)
        result.append(resolved)
    return result


def dependency_file_contains(depfile: Path, target_header: Path) -> bool:
    """Return whether Clang's dependency output proves the target was entered."""
    try:
        text = depfile.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    flattened = text.replace("\\\n", " ").replace("\\ ", " ")
    normalized = flattened.replace("\\", "/")
    target = str(target_header.resolve()).replace("\\", "/")
    return target in normalized


def explicit_inactive_reason(
    message: str,
    *,
    target_header: Path,
    sdk_root: Path,
    libcxx_root: Path | None,
) -> str | None:
    """Return fail-closed evidence that a failing physical leaf is non-surface.

    This is intentionally narrow. It accepts only an SDK-authored #error that
    actually fired for the target header, or an include written by the target
    header whose referenced file is absent from the installed SDK. Ordinary
    parse/type errors are never converted into inactivity here.
    """
    text = _read_text(target_header)
    relative = relative_sdk_path(target_header, sdk_root)
    relevant_lines = [
        line
        for line in message.splitlines()
        if relative in line or target_header.name in line
    ]
    relevant_text = "\n".join(relevant_lines)

    for raw_payload in _PP_ERROR.findall(text):
        payload = raw_payload.strip().strip("\"'").strip()
        if payload and payload.lower() in relevant_text.lower():
            return (
                "SDK header explicitly rejects the selected compilation context: "
                f"{payload}"
            )

    target_specs = {
        match.group(2).strip()
        for match in _INCLUDE_DIRECTIVE.finditer(text)
        if match.group(2).strip()
    }
    for spec in _MISSING_HEADER.findall(message):
        if spec not in target_specs:
            continue
        resolved = _resolve_include(
            source=target_header,
            spec=spec,
            quoted=False,
            sdk_root=sdk_root,
            libcxx_root=libcxx_root,
        )
        if resolved is None:
            return (
                "SDK header references a dependency absent from this SDK installation: "
                f"{spec}"
            )
    return None


def is_unreferenced_libcxx_support_header(
    path: Path,
    *,
    sdk_root: Path,
    libcxx_root: Path | None,
) -> bool:
    """Identify an unreachable libc++ support leaf after graph lookup fails.

    libc++ ships support implementations for many non-Darwin platforms inside
    every installation. A support leaf that fails the selected target, and is
    not textually reachable from any installed libc++/C header, is explicit
    non-surface evidence rather than a reason to invent prerequisites.
    """
    if libcxx_root is None:
        return False
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(libcxx_root.resolve())
    except ValueError:
        return False
    if len(relative.parts) < 3 or relative.parts[0] != "__support":
        return False
    index = _reverse_include_index(sdk_root, libcxx_root)
    return not index.get(_key(resolved))
