#!/usr/bin/env python3
"""Join ipaSim's three compatibility metadata surfaces into one typed inventory.

This tool consumes:

* the ARM64 Mach-O import manifest from ``compat_surface.py``;
* the Apple TAPI SDK surface from ``tbd_surface.py``; and
* the Clang-backed header signature surface from ``header_surface.py``.

The result is deterministic evidence about what an image requires, where the SDK
says a symbol is exported/re-exported, and whether an exact C ABI signature is
known.  It does not infer runtime behavior, implementation correctness, or a
callable bridge.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import deque
from copy import deepcopy
from pathlib import Path
from typing import Iterable, Sequence


class InventoryError(ValueError):
    """Raised when input manifests cannot be joined safely."""


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise InventoryError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise InventoryError(f"{label} must be a JSON array")
    return value


def _string(value, label: str, *, allow_none: bool = False) -> str | None:
    if value is None and allow_none:
        return None
    if not isinstance(value, str) or not value:
        raise InventoryError(f"{label} must be a non-empty string")
    return value


def _integer(value, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise InventoryError(f"{label} must be an integer")
    return value


def _boolean(value, label: str) -> bool:
    if not isinstance(value, bool):
        raise InventoryError(f"{label} must be a boolean")
    return value


def _schema(manifest: dict, label: str, *, kind: str | None = None) -> None:
    version = manifest.get("schema_version")
    if version != 1:
        raise InventoryError(
            f"{label} schema_version must be 1, got {version!r}"
        )
    if kind is not None and manifest.get("kind") != kind:
        raise InventoryError(
            f"{label} kind must be {kind!r}, got {manifest.get('kind')!r}"
        )


def load_manifest(path: Path, label: str) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise InventoryError(
            f"could not read {label} manifest {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise InventoryError(
            f"{label} manifest {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, f"{label} manifest")


def _infer_tapi_target(header_target: str) -> str:
    target = header_target.lower()
    if target.startswith("arm64e-"):
        arch = "arm64e"
    elif target.startswith("arm64-"):
        arch = "arm64"
    else:
        raise InventoryError(
            f"cannot infer TAPI target from Clang target {header_target!r}; "
            "pass --tapi-target explicitly"
        )
    if "-apple-ios" not in target:
        raise InventoryError(
            f"cannot infer iOS TAPI target from Clang target {header_target!r}; "
            "pass --tapi-target explicitly"
        )
    return f"{arch}-ios"


def _validate_macho(manifest: dict) -> None:
    _schema(manifest, "Mach-O")
    images = _list(manifest.get("images"), "Mach-O images")
    seen_images: set[str] = set()
    for image_index, raw_image in enumerate(images):
        image = _mapping(raw_image, f"Mach-O images[{image_index}]")
        name = _string(image.get("image"), f"Mach-O images[{image_index}].image")
        if name in seen_images:
            raise InventoryError(f"duplicate Mach-O image name {name!r}")
        seen_images.add(name)
        imports = _list(
            image.get("imports"), f"Mach-O image {name!r} imports"
        )
        for import_index, raw_import in enumerate(imports):
            item = _mapping(
                raw_import, f"Mach-O image {name!r} imports[{import_index}]"
            )
            _string(item.get("symbol"), f"Mach-O import symbol in {name!r}")
            _integer(item.get("ordinal"), f"Mach-O import ordinal in {name!r}")
            _string(
                item.get("provider"),
                f"Mach-O import provider in {name!r}",
                allow_none=True,
            )
            _string(
                item.get("provider_kind"),
                f"Mach-O import provider_kind in {name!r}",
            )
            _boolean(item.get("weak"), f"Mach-O import weak flag in {name!r}")
            _integer(item.get("addend"), f"Mach-O import addend in {name!r}")


def _validate_tapi(manifest: dict) -> None:
    _schema(manifest, "TAPI", kind="tapi-sdk-surface")
    interfaces = _list(manifest.get("interfaces"), "TAPI interfaces")
    seen_interfaces: set[str] = set()
    for index, raw_interface in enumerate(interfaces):
        interface = _mapping(raw_interface, f"TAPI interfaces[{index}]")
        name = _string(
            interface.get("install_name"),
            f"TAPI interfaces[{index}].install_name",
        )
        if name in seen_interfaces:
            raise InventoryError(f"duplicate TAPI interface {name!r}")
        seen_interfaces.add(name)
        _list(interface.get("targets"), f"TAPI interface {name!r} targets")
        _list(interface.get("exports"), f"TAPI interface {name!r} exports")
        _list(interface.get("reexports"), f"TAPI interface {name!r} reexports")
    _list(manifest.get("symbol_index"), "TAPI symbol_index")


def _validate_headers(manifest: dict) -> None:
    _schema(manifest, "header", kind="header-signature-surface")
    _string(manifest.get("target"), "header target")
    signatures = _list(manifest.get("signatures"), "header signatures")
    seen: set[str] = set()
    for index, raw_signature in enumerate(signatures):
        signature = _mapping(raw_signature, f"header signatures[{index}]")
        symbol = _string(
            signature.get("symbol"), f"header signatures[{index}].symbol"
        )
        if symbol in seen:
            raise InventoryError(f"duplicate header signature for {symbol!r}")
        seen.add(symbol)
        _boolean(signature.get("variadic"), f"header signature {symbol!r} variadic")
        _boolean(signature.get("prototype"), f"header signature {symbol!r} prototype")
        _mapping(
            signature.get("return_type"),
            f"header signature {symbol!r} return_type",
        )
        _list(
            signature.get("parameters"),
            f"header signature {symbol!r} parameters",
        )


def _targeted(values: object, target: str, label: str) -> bool:
    targets = _list(values, label)
    for item in targets:
        if not isinstance(item, str):
            raise InventoryError(f"{label} contains a non-string target")
    return target in targets


def _build_tapi_indexes(
    manifest: dict, target: str
) -> tuple[dict[str, list[dict]], dict[str, tuple[str, ...]]]:
    """Return direct global exports and the explicit re-export graph."""
    graph_sets: dict[str, set[str]] = {}
    for raw_interface in manifest["interfaces"]:
        interface = _mapping(raw_interface, "TAPI interface")
        install_name = _string(interface.get("install_name"), "TAPI install_name")
        if not _targeted(
            interface.get("targets"), target, f"TAPI {install_name} targets"
        ):
            continue
        outgoing = graph_sets.setdefault(install_name, set())
        for raw_reexport in _list(
            interface.get("reexports"), f"TAPI {install_name} reexports"
        ):
            reexport = _mapping(raw_reexport, f"TAPI {install_name} reexport")
            child = _string(
                reexport.get("install_name"),
                f"TAPI {install_name} reexport install_name",
            )
            if _targeted(
                reexport.get("targets"),
                target,
                f"TAPI {install_name} -> {child} targets",
            ):
                outgoing.add(child)

    exports: dict[str, list[dict]] = {}
    seen_facts: set[tuple[str, str, bool]] = set()
    for raw_symbol in manifest["symbol_index"]:
        symbol_entry = _mapping(raw_symbol, "TAPI symbol_index entry")
        symbol = _string(symbol_entry.get("name"), "TAPI symbol name")
        kind = _string(symbol_entry.get("kind"), f"TAPI symbol {symbol!r} kind")
        weak = _boolean(
            symbol_entry.get("weak"), f"TAPI symbol {symbol!r} weak flag"
        )
        # The current header analyzer emits C ABI function symbols only.  Do not
        # conflate Objective-C metadata or thread-local records with C functions.
        if kind != "global":
            continue
        for raw_provider in _list(
            symbol_entry.get("providers"), f"TAPI symbol {symbol!r} providers"
        ):
            provider = _mapping(raw_provider, f"TAPI symbol {symbol!r} provider")
            install_name = _string(
                provider.get("install_name"),
                f"TAPI symbol {symbol!r} provider install_name",
            )
            if not _targeted(
                provider.get("targets"),
                target,
                f"TAPI symbol {symbol!r} provider {install_name!r} targets",
            ):
                continue
            fact_key = (symbol, install_name, weak)
            if fact_key in seen_facts:
                continue
            seen_facts.add(fact_key)
            exports.setdefault(symbol, []).append(
                {
                    "install_name": install_name,
                    "weak": weak,
                }
            )

    for symbol in exports:
        exports[symbol].sort(
            key=lambda item: (item["install_name"], item["weak"])
        )
    graph = {
        name: tuple(sorted(children))
        for name, children in sorted(graph_sets.items())
    }
    return exports, graph


def _build_signature_index(manifest: dict) -> dict[str, dict]:
    return {
        _string(item.get("symbol"), "header signature symbol"): deepcopy(item)
        for item in manifest["signatures"]
    }


def _find_reexport_path(
    start: str, destinations: set[str], graph: dict[str, tuple[str, ...]]
) -> list[str] | None:
    if start in destinations:
        return [start]
    queue: deque[tuple[str, ...]] = deque([(start,)])
    visited = {start}
    while queue:
        path = queue.popleft()
        node = path[-1]
        for child in graph.get(node, ()):
            if child in visited:
                continue
            next_path = path + (child,)
            if child in destinations:
                return list(next_path)
            visited.add(child)
            queue.append(next_path)
    return None


def _provider_match(
    expected_provider: str | None,
    provider_kind: str,
    sdk_exports: Sequence[dict],
    graph: dict[str, tuple[str, ...]],
) -> dict:
    if expected_provider is None:
        return {
            "kind": "special-ordinal",
            "path": [],
            "special_provider_kind": provider_kind,
        }

    direct_providers = {
        _string(item.get("install_name"), "SDK export provider")
        for item in sdk_exports
    }
    if not direct_providers:
        return {
            "kind": "sdk-symbol-absent",
            "path": [],
            "special_provider_kind": None,
        }
    if expected_provider in direct_providers:
        return {
            "kind": "direct",
            "path": [expected_provider],
            "special_provider_kind": None,
        }
    path = _find_reexport_path(expected_provider, direct_providers, graph)
    if path is not None:
        return {
            "kind": "reexport",
            "path": path,
            "special_provider_kind": None,
        }
    return {
        "kind": "provider-mismatch",
        "path": [],
        "special_provider_kind": None,
    }


def build_inventory(
    macho_manifest: dict,
    tapi_manifest: dict,
    header_manifest: dict,
    *,
    tapi_target: str | None = None,
) -> dict:
    """Join three validated schema-v1 manifests into one deterministic inventory."""
    _validate_macho(macho_manifest)
    _validate_tapi(tapi_manifest)
    _validate_headers(header_manifest)

    clang_target = _string(header_manifest.get("target"), "header target")
    resolved_tapi_target = tapi_target or _infer_tapi_target(clang_target)
    if not isinstance(resolved_tapi_target, str) or not resolved_tapi_target:
        raise InventoryError("TAPI target must be a non-empty string")

    sdk_exports_by_symbol, reexport_graph = _build_tapi_indexes(
        tapi_manifest, resolved_tapi_target
    )
    signatures = _build_signature_index(header_manifest)

    requirements: list[dict] = []
    required_by_symbol: dict[str, set[str]] = {}
    counts = {
        "typed_requirement_count": 0,
        "untyped_requirement_count": 0,
        "direct_provider_match_count": 0,
        "reexport_provider_match_count": 0,
        "provider_mismatch_count": 0,
        "sdk_symbol_absent_count": 0,
        "special_ordinal_count": 0,
        "weak_import_count": 0,
        "variadic_typed_requirement_count": 0,
        "no_prototype_typed_requirement_count": 0,
    }

    for raw_image in macho_manifest["images"]:
        image = _mapping(raw_image, "Mach-O image")
        image_name = _string(image.get("image"), "Mach-O image name")
        imports = _list(image.get("imports"), f"Mach-O image {image_name} imports")
        for raw_import in imports:
            item = _mapping(raw_import, f"Mach-O import in {image_name}")
            symbol = _string(item.get("symbol"), "Mach-O import symbol")
            expected_provider = _string(
                item.get("provider"), "Mach-O import provider", allow_none=True
            )
            provider_kind = _string(
                item.get("provider_kind"), "Mach-O import provider_kind"
            )
            sdk_exports = deepcopy(sdk_exports_by_symbol.get(symbol, []))
            match = _provider_match(
                expected_provider, provider_kind, sdk_exports, reexport_graph
            )
            signature = signatures.get(symbol)
            signature_known = signature is not None

            if signature_known:
                counts["typed_requirement_count"] += 1
                if bool(signature.get("variadic")):
                    counts["variadic_typed_requirement_count"] += 1
                if not bool(signature.get("prototype")):
                    counts["no_prototype_typed_requirement_count"] += 1
            else:
                counts["untyped_requirement_count"] += 1

            match_kind = match["kind"]
            if match_kind == "direct":
                counts["direct_provider_match_count"] += 1
            elif match_kind == "reexport":
                counts["reexport_provider_match_count"] += 1
            elif match_kind == "provider-mismatch":
                counts["provider_mismatch_count"] += 1
            elif match_kind == "sdk-symbol-absent":
                counts["sdk_symbol_absent_count"] += 1
            elif match_kind == "special-ordinal":
                counts["special_ordinal_count"] += 1
            else:  # defensive: _provider_match is intentionally closed-set
                raise InventoryError(f"unknown provider match kind {match_kind!r}")

            weak = _boolean(item.get("weak"), "Mach-O import weak flag")
            if weak:
                counts["weak_import_count"] += 1

            required_by_symbol.setdefault(symbol, set()).add(image_name)
            requirements.append(
                {
                    "image": image_name,
                    "symbol": symbol,
                    "ordinal": _integer(item.get("ordinal"), "Mach-O import ordinal"),
                    "expected_provider": expected_provider,
                    "provider_kind": provider_kind,
                    "weak_import": weak,
                    "addend": _integer(item.get("addend"), "Mach-O import addend"),
                    "header_signature_available": signature_known,
                    "sdk_direct_exports": sdk_exports,
                    "provider_match": match,
                }
            )

    requirements.sort(
        key=lambda item: (
            item["image"],
            item["symbol"],
            item["expected_provider"] or "",
            item["ordinal"],
            item["weak_import"],
            item["addend"],
        )
    )

    symbols = []
    typed_unique = 0
    for symbol in sorted(required_by_symbol):
        signature = deepcopy(signatures.get(symbol))
        if signature is not None:
            typed_unique += 1
        requirement_count = sum(
            1 for item in requirements if item["symbol"] == symbol
        )
        symbols.append(
            {
                "symbol": symbol,
                "required_by": sorted(required_by_symbol[symbol]),
                "requirement_count": requirement_count,
                "sdk_direct_exports": deepcopy(
                    sdk_exports_by_symbol.get(symbol, [])
                ),
                "signature": signature,
            }
        )

    return {
        "schema_version": 1,
        "kind": "typed-compatibility-inventory",
        "targets": {
            "clang": clang_target,
            "tapi": resolved_tapi_target,
        },
        "summary": {
            "requirement_count": len(requirements),
            "unique_required_symbol_count": len(symbols),
            "typed_unique_symbol_count": typed_unique,
            "untyped_unique_symbol_count": len(symbols) - typed_unique,
            **counts,
        },
        "symbols": symbols,
        "requirements": requirements,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--macho",
        required=True,
        help="Mach-O compatibility-surface JSON from compat_surface.py",
    )
    parser.add_argument(
        "--tapi",
        required=True,
        help="TAPI SDK-surface JSON from tbd_surface.py",
    )
    parser.add_argument(
        "--headers",
        required=True,
        help="Clang header-signature JSON from header_surface.py",
    )
    parser.add_argument(
        "--tapi-target",
        help="TAPI target such as arm64-ios; inferred from header target by default",
    )
    parser.add_argument(
        "--output",
        help="write JSON inventory to this file instead of stdout",
    )
    args = parser.parse_args(argv)

    try:
        inventory = build_inventory(
            load_manifest(Path(args.macho), "Mach-O"),
            load_manifest(Path(args.tapi), "TAPI"),
            load_manifest(Path(args.headers), "header"),
            tapi_target=args.tapi_target,
        )
        rendered = json.dumps(inventory, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (InventoryError, OSError) as exc:
        print(f"[inventory-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
