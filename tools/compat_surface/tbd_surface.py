#!/usr/bin/env python3
"""Apple TAPI .tbd knowledge extractor for ipaSim.

This tool is metadata-only. It normalizes ARM64/ARM64e iOS export and re-export
information from legacy and modern TAPI text stubs into deterministic JSON.
It never infers function signatures or implementation semantics.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    import yaml
except ImportError as exc:  # pragma: no cover - exercised by CLI environment
    yaml = None
    _YAML_IMPORT_ERROR = exc
else:
    _YAML_IMPORT_ERROR = None

_DEFAULT_TARGETS = ("arm64-ios", "arm64e-ios")

_SYMBOL_KEYS = {
    "symbols": ("global", False),
    "weak-symbols": ("global", True),
    "weak-def-symbols": ("global", True),
    "thread-local-symbols": ("thread-local", False),
    "objc-classes": ("objc-class", False),
    "objc-eh-types": ("objc-eh-type", False),
    "objc-ivars": ("objc-ivar", False),
}


class TbdParseError(ValueError):
    """Raised when a text stub cannot be normalized safely."""


@dataclass(frozen=True)
class TbdExport:
    name: str
    kind: str
    weak: bool
    targets: tuple[str, ...]


@dataclass(frozen=True)
class TbdReexport:
    install_name: str
    targets: tuple[str, ...]


@dataclass(frozen=True)
class TbdInterface:
    install_name: str
    format_version: int
    current_version: str | None
    compatibility_version: str | None
    targets: tuple[str, ...]
    exports: tuple[TbdExport, ...]
    reexports: tuple[TbdReexport, ...]
    sources: tuple[str, ...]


def _require_yaml() -> None:
    if yaml is None:
        raise TbdParseError(
            "PyYAML is required for TAPI text-stub parsing; install PyYAML==6.0.2"
        ) from _YAML_IMPORT_ERROR


def _make_loader():
    _require_yaml()

    class TbdLoader(yaml.SafeLoader):
        pass

    def construct_unknown(loader, node):
        if isinstance(node, yaml.MappingNode):
            value = loader.construct_mapping(node, deep=True)
            value["__tapi_tag__"] = node.tag
            return value
        if isinstance(node, yaml.SequenceNode):
            return loader.construct_sequence(node, deep=True)
        return loader.construct_scalar(node)

    TbdLoader.add_constructor(None, construct_unknown)
    return TbdLoader


def _as_string(value) -> str | None:
    if value is None:
        return None
    return str(value)


def _format_version(document: dict) -> int:
    raw = document.get("tbd-version")
    if raw is not None:
        try:
            return int(raw)
        except (TypeError, ValueError) as exc:
            raise TbdParseError(f"invalid tbd-version {raw!r}") from exc

    tag = str(document.get("__tapi_tag__", ""))
    for suffix, version in (("-v3", 3), ("-v2", 2), ("-v1", 1)):
        if tag.endswith(suffix):
            return version
    # Untagged early text stubs predate explicit tbd-version metadata.
    return 1


def _normalize_requested_targets(targets: Sequence[str] | None) -> tuple[str, ...]:
    raw = targets or _DEFAULT_TARGETS
    cleaned = tuple(sorted({str(item).strip() for item in raw if str(item).strip()}))
    if not cleaned:
        raise TbdParseError("at least one target must be requested")
    return cleaned


def _legacy_target_name(arch: str, platform: str) -> str:
    return f"{arch}-{platform}"


def _matching_document_targets(
    document: dict, requested: Sequence[str]
) -> tuple[str, ...]:
    if "targets" in document:
        available = {str(item) for item in document.get("targets") or []}
        return tuple(sorted(available.intersection(requested)))

    platform = str(document.get("platform", "ios"))
    if platform != "ios":
        return ()
    available = {
        _legacy_target_name(str(arch), platform)
        for arch in (document.get("archs") or [])
    }
    return tuple(sorted(available.intersection(requested)))


def _matching_group_targets(
    group: dict,
    document: dict,
    requested: Sequence[str],
    document_targets: Sequence[str],
) -> tuple[str, ...]:
    if "targets" in group:
        available = {str(item) for item in group.get("targets") or []}
        return tuple(sorted(available.intersection(requested)))

    if "archs" in group:
        platform = str(document.get("platform", "ios"))
        if platform != "ios":
            return ()
        available = {
            _legacy_target_name(str(arch), platform)
            for arch in (group.get("archs") or [])
        }
        return tuple(sorted(available.intersection(requested)))

    return tuple(document_targets)


def _merge_target_map(mapping: dict, key, targets: Iterable[str]) -> None:
    mapping.setdefault(key, set()).update(targets)


def _parse_document(
    document: dict,
    source: str,
    requested_targets: Sequence[str],
) -> TbdInterface | None:
    if not isinstance(document, dict):
        raise TbdParseError(f"{source}: TAPI document is not a mapping")

    install_name = document.get("install-name")
    if not isinstance(install_name, str) or not install_name:
        raise TbdParseError(f"{source}: TAPI document has no valid install-name")

    document_targets = _matching_document_targets(document, requested_targets)
    if not document_targets:
        return None

    export_targets: dict[tuple[str, str, bool], set[str]] = {}
    reexport_targets: dict[str, set[str]] = {}

    export_groups = document.get("exports") or []
    if not isinstance(export_groups, list):
        raise TbdParseError(f"{source}: exports must be a list")

    for group in export_groups:
        if not isinstance(group, dict):
            raise TbdParseError(f"{source}: export group is not a mapping")
        targets = _matching_group_targets(
            group, document, requested_targets, document_targets
        )
        if not targets:
            continue

        for key, (kind, weak) in _SYMBOL_KEYS.items():
            values = group.get(key) or []
            if not isinstance(values, list):
                raise TbdParseError(f"{source}: {key} must be a list")
            for name in values:
                if not isinstance(name, str) or not name:
                    raise TbdParseError(f"{source}: {key} contains an invalid name")
                _merge_target_map(export_targets, (name, kind, weak), targets)

        # Legacy v1-v3 stubs express umbrella re-exports inside export groups.
        values = group.get("re-exports") or []
        if not isinstance(values, list):
            raise TbdParseError(f"{source}: re-exports must be a list")
        for library in values:
            if not isinstance(library, str) or not library:
                raise TbdParseError(f"{source}: re-exports contains an invalid library")
            _merge_target_map(reexport_targets, library, targets)

    # TAPI v4 moves re-exported libraries into their own top-level groups.
    reexport_groups = document.get("reexported-libraries") or []
    if not isinstance(reexport_groups, list):
        raise TbdParseError(f"{source}: reexported-libraries must be a list")
    for group in reexport_groups:
        if not isinstance(group, dict):
            raise TbdParseError(
                f"{source}: reexported-libraries entry is not a mapping"
            )
        targets = _matching_group_targets(
            group, document, requested_targets, document_targets
        )
        if not targets:
            continue
        libraries = group.get("libraries") or []
        if not isinstance(libraries, list):
            raise TbdParseError(f"{source}: libraries must be a list")
        for library in libraries:
            if not isinstance(library, str) or not library:
                raise TbdParseError(f"{source}: libraries contains an invalid name")
            _merge_target_map(reexport_targets, library, targets)

    exports = tuple(
        TbdExport(name, kind, weak, tuple(sorted(targets)))
        for (name, kind, weak), targets in sorted(
            export_targets.items(), key=lambda item: (item[0][0], item[0][1], item[0][2])
        )
    )
    reexports = tuple(
        TbdReexport(name, tuple(sorted(targets)))
        for name, targets in sorted(reexport_targets.items())
    )

    return TbdInterface(
        install_name=install_name,
        format_version=_format_version(document),
        current_version=_as_string(document.get("current-version")),
        compatibility_version=_as_string(document.get("compatibility-version")),
        targets=tuple(document_targets),
        exports=exports,
        reexports=reexports,
        sources=(source.replace("\\", "/"),),
    )


def parse_tbd_text(
    text: str,
    source: str = "input.tbd",
    requested_targets: Sequence[str] | None = None,
) -> list[TbdInterface]:
    """Parse a possibly multi-document TAPI text stub."""
    _require_yaml()
    targets = _normalize_requested_targets(requested_targets)
    Loader = _make_loader()
    try:
        documents = list(yaml.load_all(text, Loader=Loader))
    except yaml.YAMLError as exc:
        raise TbdParseError(f"{source}: invalid YAML/TAPI text stub: {exc}") from exc

    interfaces: list[TbdInterface] = []
    for document in documents:
        if document is None:
            continue
        interface = _parse_document(document, source, targets)
        if interface is not None:
            interfaces.append(interface)
    return interfaces


def parse_tbd_file(
    path: Path,
    display_name: str | None = None,
    requested_targets: Sequence[str] | None = None,
) -> list[TbdInterface]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise TbdParseError(
            f"could not read {display_name or path.name}: {exc.strerror or exc}"
        ) from exc
    except UnicodeDecodeError as exc:
        raise TbdParseError(f"{display_name or path.name}: text stub is not UTF-8") from exc
    return parse_tbd_text(text, display_name or path.name, requested_targets)


def _merge_interface_group(group: Sequence[TbdInterface]) -> TbdInterface:
    first = group[0]
    versions = {item.format_version for item in group}
    current_versions = {item.current_version for item in group if item.current_version}
    compatibility_versions = {
        item.compatibility_version for item in group if item.compatibility_version
    }
    if len(versions) != 1:
        raise TbdParseError(
            f"{first.install_name}: conflicting TAPI format versions {sorted(versions)}"
        )
    if len(current_versions) > 1:
        raise TbdParseError(
            f"{first.install_name}: conflicting current-version values "
            f"{sorted(current_versions)}"
        )
    if len(compatibility_versions) > 1:
        raise TbdParseError(
            f"{first.install_name}: conflicting compatibility-version values "
            f"{sorted(compatibility_versions)}"
        )

    export_targets: dict[tuple[str, str, bool], set[str]] = {}
    reexport_targets: dict[str, set[str]] = {}
    all_targets: set[str] = set()
    sources: set[str] = set()
    for item in group:
        all_targets.update(item.targets)
        sources.update(item.sources)
        for export in item.exports:
            _merge_target_map(
                export_targets, (export.name, export.kind, export.weak), export.targets
            )
        for reexport in item.reexports:
            _merge_target_map(reexport_targets, reexport.install_name, reexport.targets)

    exports = tuple(
        TbdExport(name, kind, weak, tuple(sorted(targets)))
        for (name, kind, weak), targets in sorted(
            export_targets.items(), key=lambda item: (item[0][0], item[0][1], item[0][2])
        )
    )
    reexports = tuple(
        TbdReexport(name, tuple(sorted(targets)))
        for name, targets in sorted(reexport_targets.items())
    )
    return TbdInterface(
        install_name=first.install_name,
        format_version=first.format_version,
        current_version=next(iter(current_versions), None),
        compatibility_version=next(iter(compatibility_versions), None),
        targets=tuple(sorted(all_targets)),
        exports=exports,
        reexports=reexports,
        sources=tuple(sorted(sources)),
    )


def merge_interfaces(interfaces: Iterable[TbdInterface]) -> list[TbdInterface]:
    groups: dict[str, list[TbdInterface]] = {}
    for interface in interfaces:
        groups.setdefault(interface.install_name, []).append(interface)
    return [
        _merge_interface_group(groups[name])
        for name in sorted(groups)
    ]


def build_sdk_manifest(interfaces: Iterable[TbdInterface]) -> dict:
    merged = merge_interfaces(list(interfaces))
    weak_count = 0
    objc_count = 0
    export_count = 0
    reexport_count = 0
    symbol_providers: dict[tuple[str, str, bool], dict[str, set[str]]] = {}

    rendered = []
    for interface in merged:
        export_count += len(interface.exports)
        reexport_count += len(interface.reexports)
        weak_count += sum(1 for item in interface.exports if item.weak)
        objc_count += sum(1 for item in interface.exports if item.kind.startswith("objc-"))

        rendered_exports = []
        for item in interface.exports:
            rendered_exports.append(
                {
                    "name": item.name,
                    "kind": item.kind,
                    "weak": item.weak,
                    "targets": list(item.targets),
                }
            )
            providers = symbol_providers.setdefault(
                (item.name, item.kind, item.weak), {}
            )
            providers.setdefault(interface.install_name, set()).update(item.targets)

        rendered.append(
            {
                "install_name": interface.install_name,
                "format_version": interface.format_version,
                "current_version": interface.current_version,
                "compatibility_version": interface.compatibility_version,
                "targets": list(interface.targets),
                "sources": list(interface.sources),
                "exports": rendered_exports,
                "reexports": [
                    {
                        "install_name": item.install_name,
                        "targets": list(item.targets),
                    }
                    for item in interface.reexports
                ],
            }
        )

    symbol_index = []
    for (name, kind, weak), providers in sorted(
        symbol_providers.items(), key=lambda item: (item[0][0], item[0][1], item[0][2])
    ):
        symbol_index.append(
            {
                "name": name,
                "kind": kind,
                "weak": weak,
                "providers": [
                    {
                        "install_name": provider,
                        "targets": sorted(targets),
                    }
                    for provider, targets in sorted(providers.items())
                ],
            }
        )

    return {
        "schema_version": 1,
        "kind": "tapi-sdk-surface",
        "summary": {
            "interface_count": len(merged),
            "export_count": export_count,
            "weak_export_count": weak_count,
            "objc_export_count": objc_count,
            "reexport_count": reexport_count,
            "unique_symbol_count": len(symbol_index),
        },
        "interfaces": rendered,
        "symbol_index": symbol_index,
    }


def _collect_inputs(
    positional: Sequence[str], sdk_root: str | None
) -> list[tuple[Path, str]]:
    inputs: list[tuple[Path, str]] = []

    if sdk_root:
        root = Path(sdk_root).resolve()
        if not root.is_dir():
            raise TbdParseError(f"SDK root is not a directory: {root}")
        for path in sorted(root.rglob("*.tbd")):
            inputs.append((path, path.relative_to(root).as_posix()))

    for raw in positional:
        path = Path(raw).resolve()
        if path.is_dir():
            for child in sorted(path.rglob("*.tbd")):
                inputs.append((child, child.relative_to(path).as_posix()))
        elif path.is_file():
            inputs.append((path, path.name))
        else:
            raise TbdParseError(f"input does not exist: {raw}")

    # Preserve display-name identity but avoid analyzing the same resolved file twice.
    seen: set[Path] = set()
    unique = []
    for path, display in inputs:
        if path in seen:
            continue
        seen.add(path)
        unique.append((path, display))
    if not unique:
        raise TbdParseError("no .tbd inputs were found")
    return unique


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Normalize Apple TAPI .tbd metadata for ipaSim compatibility analysis."
    )
    parser.add_argument("tbd", nargs="*", help=".tbd file or directory to scan recursively")
    parser.add_argument(
        "--sdk-root",
        help="SDK root to scan recursively; manifest source paths remain relative to it",
    )
    parser.add_argument(
        "--target",
        action="append",
        dest="targets",
        help="TAPI target to include (repeatable; default arm64-ios and arm64e-ios)",
    )
    parser.add_argument("--output", help="write JSON to this file instead of stdout")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        targets = _normalize_requested_targets(args.targets)
        interfaces: list[TbdInterface] = []
        for path, display in _collect_inputs(args.tbd, args.sdk_root):
            interfaces.extend(parse_tbd_file(path, display, targets))
        manifest = build_sdk_manifest(interfaces)
    except TbdParseError as exc:
        print(f"[tbd-surface] ERROR: {exc}", file=sys.stderr)
        return 2

    rendered = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
    if args.output:
        try:
            Path(args.output).write_text(rendered, encoding="utf-8")
        except OSError as exc:
            print(f"[tbd-surface] ERROR: could not write output: {exc}", file=sys.stderr)
            return 2
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
