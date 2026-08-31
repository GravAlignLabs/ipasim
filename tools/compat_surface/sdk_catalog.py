#!/usr/bin/env python3
"""Build a deterministic SDK-wide typed compatibility catalog for ipaSim.

Unlike ``inventory_surface.py``, this tool is not gated on imports observed in a
particular Mach-O image. It joins the complete target-matching Apple TAPI symbol
surface with exact Clang-backed C signatures so the mechanical iOS API/ABI
surface can be reasoned about and generated in bulk.

The catalog is evidence only. TAPI metadata establishes export/provider facts;
Clang establishes C type facts. Neither source grants semantic compatibility,
selects a Windows implementation, or approves a runtime route.

For the existing AAPCS64 lowering tool, this module can also emit a deliberately
mechanical projection using the pre-existing ``typed-compatibility-inventory``
shape. That projection contains every typed global C export in the SDK catalog,
not just symbols observed in one Mach-O. It exists only to feed the already
validated ABI generator and contains no runtime requirements or semantic claims.
"""

from __future__ import annotations

import argparse
import json
import sys
from copy import deepcopy
from pathlib import Path
from typing import Sequence


class SdkCatalogError(ValueError):
    """Raised when SDK evidence cannot be joined safely."""


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise SdkCatalogError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise SdkCatalogError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise SdkCatalogError(f"{label} must be a non-empty string")
    return value


def _boolean(value, label: str) -> bool:
    if not isinstance(value, bool):
        raise SdkCatalogError(f"{label} must be a boolean")
    return value


def _schema(manifest: dict, label: str, kind: str) -> None:
    if manifest.get("schema_version") != 1:
        raise SdkCatalogError(
            f"{label} schema_version must be 1, got {manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != kind:
        raise SdkCatalogError(
            f"{label} kind must be {kind!r}, got {manifest.get('kind')!r}"
        )


def load_manifest(path: Path, label: str) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise SdkCatalogError(
            f"could not read {label} manifest {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SdkCatalogError(
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
        raise SdkCatalogError(
            f"cannot infer TAPI target from Clang target {header_target!r}; "
            "pass --tapi-target explicitly"
        )
    if "-apple-ios" not in target:
        raise SdkCatalogError(
            f"cannot infer iOS TAPI target from Clang target {header_target!r}; "
            "pass --tapi-target explicitly"
        )
    return f"{arch}-ios"


def _validate_targets(values: object, label: str) -> list[str]:
    targets = _list(values, label)
    rendered = []
    for index, value in enumerate(targets):
        rendered.append(_string(value, f"{label}[{index}]"))
    return rendered


def _validate_tapi(manifest: dict) -> None:
    _schema(manifest, "TAPI", "tapi-sdk-surface")
    interfaces = _list(manifest.get("interfaces"), "TAPI interfaces")
    seen_interfaces: set[str] = set()
    for index, raw_interface in enumerate(interfaces):
        interface = _mapping(raw_interface, f"TAPI interfaces[{index}]")
        name = _string(
            interface.get("install_name"),
            f"TAPI interfaces[{index}].install_name",
        )
        if name in seen_interfaces:
            raise SdkCatalogError(f"duplicate TAPI interface {name!r}")
        seen_interfaces.add(name)
        _validate_targets(interface.get("targets"), f"TAPI interface {name!r} targets")
        _list(interface.get("exports"), f"TAPI interface {name!r} exports")
        _list(interface.get("reexports"), f"TAPI interface {name!r} reexports")

    symbol_index = _list(manifest.get("symbol_index"), "TAPI symbol_index")
    for index, raw_entry in enumerate(symbol_index):
        entry = _mapping(raw_entry, f"TAPI symbol_index[{index}]")
        symbol = _string(entry.get("name"), f"TAPI symbol_index[{index}].name")
        _string(entry.get("kind"), f"TAPI symbol {symbol!r} kind")
        _boolean(entry.get("weak"), f"TAPI symbol {symbol!r} weak")
        providers = _list(entry.get("providers"), f"TAPI symbol {symbol!r} providers")
        if not providers:
            raise SdkCatalogError(f"TAPI symbol {symbol!r} has no direct providers")
        for provider_index, raw_provider in enumerate(providers):
            provider = _mapping(
                raw_provider,
                f"TAPI symbol {symbol!r} providers[{provider_index}]",
            )
            install_name = _string(
                provider.get("install_name"),
                f"TAPI symbol {symbol!r} provider install_name",
            )
            _validate_targets(
                provider.get("targets"),
                f"TAPI symbol {symbol!r} provider {install_name!r} targets",
            )


def _validate_headers(manifest: dict) -> None:
    _schema(manifest, "header", "header-signature-surface")
    _string(manifest.get("target"), "header target")
    signatures = _list(manifest.get("signatures"), "header signatures")
    seen: set[str] = set()
    for index, raw_signature in enumerate(signatures):
        signature = _mapping(raw_signature, f"header signatures[{index}]")
        symbol = _string(
            signature.get("symbol"), f"header signatures[{index}].symbol"
        )
        if symbol in seen:
            raise SdkCatalogError(f"duplicate header signature for {symbol!r}")
        seen.add(symbol)
        _list(signature.get("names"), f"header signature {symbol!r} names")
        _boolean(signature.get("variadic"), f"header signature {symbol!r} variadic")
        _boolean(signature.get("prototype"), f"header signature {symbol!r} prototype")
        _mapping(signature.get("return_type"), f"header signature {symbol!r} return_type")
        _list(signature.get("parameters"), f"header signature {symbol!r} parameters")
        _list(signature.get("sources"), f"header signature {symbol!r} sources")


def _signature_index(manifest: dict) -> dict[str, dict]:
    return {
        _string(item.get("symbol"), "header signature symbol"): deepcopy(item)
        for item in manifest["signatures"]
    }


def _matching_provider_facts(entry: dict, target: str) -> list[dict]:
    symbol = _string(entry.get("name"), "TAPI symbol name")
    kind = _string(entry.get("kind"), f"TAPI symbol {symbol!r} kind")
    weak = _boolean(entry.get("weak"), f"TAPI symbol {symbol!r} weak")
    facts = []
    for raw_provider in _list(
        entry.get("providers"), f"TAPI symbol {symbol!r} providers"
    ):
        provider = _mapping(raw_provider, f"TAPI symbol {symbol!r} provider")
        install_name = _string(
            provider.get("install_name"),
            f"TAPI symbol {symbol!r} provider install_name",
        )
        targets = sorted(
            set(
                _validate_targets(
                    provider.get("targets"),
                    f"TAPI symbol {symbol!r} provider {install_name!r} targets",
                )
            )
        )
        if target not in targets:
            continue
        facts.append(
            {
                "install_name": install_name,
                "kind": kind,
                "weak": weak,
                "targets": targets,
            }
        )
    return facts


def _classification(kinds: list[str], signature: dict | None) -> str:
    if kinds == ["global"]:
        return "typed-c-function" if signature is not None else "untyped-global"
    if kinds == ["thread-local"]:
        return "thread-local-data"
    if kinds and all(kind.startswith("objc-") for kind in kinds):
        return "objc-metadata"
    if len(kinds) > 1:
        return "mixed-sdk-metadata"
    return "non-c-metadata"


def build_sdk_catalog(
    tapi_manifest: dict,
    header_manifest: dict,
    *,
    tapi_target: str | None = None,
) -> dict:
    """Join all target-matching TAPI symbols with exact header signatures."""
    _validate_tapi(tapi_manifest)
    _validate_headers(header_manifest)

    clang_target = _string(header_manifest.get("target"), "header target")
    resolved_tapi_target = tapi_target or _infer_tapi_target(clang_target)
    if not isinstance(resolved_tapi_target, str) or not resolved_tapi_target:
        raise SdkCatalogError("TAPI target must be a non-empty string")

    signatures = _signature_index(header_manifest)
    grouped: dict[str, list[dict]] = {}
    for raw_entry in tapi_manifest["symbol_index"]:
        entry = _mapping(raw_entry, "TAPI symbol_index entry")
        symbol = _string(entry.get("name"), "TAPI symbol name")
        facts = _matching_provider_facts(entry, resolved_tapi_target)
        if facts:
            grouped.setdefault(symbol, []).extend(facts)

    symbols = []
    counts = {
        "global_symbol_count": 0,
        "typed_global_symbol_count": 0,
        "untyped_global_symbol_count": 0,
        "weak_symbol_count": 0,
        "objc_symbol_count": 0,
        "thread_local_symbol_count": 0,
        "mixed_kind_symbol_count": 0,
        "multi_provider_symbol_count": 0,
    }

    for symbol in sorted(grouped):
        unique_facts: dict[tuple[str, str, bool, tuple[str, ...]], dict] = {}
        for fact in grouped[symbol]:
            key = (
                fact["install_name"],
                fact["kind"],
                fact["weak"],
                tuple(fact["targets"]),
            )
            unique_facts[key] = fact
        direct_exports = [
            deepcopy(unique_facts[key])
            for key in sorted(unique_facts)
        ]
        kinds = sorted({fact["kind"] for fact in direct_exports})
        signature = deepcopy(signatures.get(symbol))
        classification = _classification(kinds, signature)
        callable_c_candidate = classification == "typed-c-function"
        provider_names = sorted({fact["install_name"] for fact in direct_exports})
        weak = any(fact["weak"] for fact in direct_exports)
        strong = any(not fact["weak"] for fact in direct_exports)

        if kinds == ["global"]:
            counts["global_symbol_count"] += 1
            if callable_c_candidate:
                counts["typed_global_symbol_count"] += 1
            else:
                counts["untyped_global_symbol_count"] += 1
        if weak:
            counts["weak_symbol_count"] += 1
        if any(kind.startswith("objc-") for kind in kinds):
            counts["objc_symbol_count"] += 1
        if "thread-local" in kinds:
            counts["thread_local_symbol_count"] += 1
        if len(kinds) > 1:
            counts["mixed_kind_symbol_count"] += 1
        if len(provider_names) > 1:
            counts["multi_provider_symbol_count"] += 1

        symbols.append(
            {
                "symbol": symbol,
                "sdk_kinds": kinds,
                "weak_export": weak,
                "strong_export": strong,
                "classification": classification,
                "callable_c_candidate": callable_c_candidate,
                "sdk_direct_exports": direct_exports,
                "signature": signature,
            }
        )

    catalog_symbols = {item["symbol"] for item in symbols}
    orphan_header_signatures = sorted(set(signatures) - catalog_symbols)

    targeted_interfaces = 0
    for raw_interface in tapi_manifest["interfaces"]:
        interface = _mapping(raw_interface, "TAPI interface")
        targets = _validate_targets(
            interface.get("targets"),
            f"TAPI interface {interface.get('install_name')!r} targets",
        )
        if resolved_tapi_target in targets:
            targeted_interfaces += 1

    return {
        "schema_version": 1,
        "kind": "typed-sdk-catalog",
        "targets": {
            "clang": clang_target,
            "tapi": resolved_tapi_target,
        },
        "summary": {
            "interface_count": targeted_interfaces,
            "symbol_count": len(symbols),
            **counts,
            "header_signature_count": len(signatures),
            "orphan_header_signature_count": len(orphan_header_signatures),
        },
        "symbols": symbols,
        "orphan_header_signatures": orphan_header_signatures,
    }


def build_abi_inventory(catalog_manifest: dict) -> dict:
    """Project SDK-wide typed C exports into the existing ABI-generator input shape.

    This is a mechanical adapter between manifest schemas. It intentionally emits
    no Mach-O requirements and does not invent semantic implementation status.
    """
    _schema(catalog_manifest, "SDK catalog", "typed-sdk-catalog")
    targets = _mapping(catalog_manifest.get("targets"), "SDK catalog targets")
    clang_target = _string(targets.get("clang"), "SDK catalog Clang target")
    tapi_target = _string(targets.get("tapi"), "SDK catalog TAPI target")

    projected = []
    seen: set[str] = set()
    for index, raw_item in enumerate(
        _list(catalog_manifest.get("symbols"), "SDK catalog symbols")
    ):
        item = _mapping(raw_item, f"SDK catalog symbols[{index}]")
        symbol = _string(item.get("symbol"), f"SDK catalog symbols[{index}].symbol")
        if symbol in seen:
            raise SdkCatalogError(f"duplicate SDK catalog symbol {symbol!r}")
        seen.add(symbol)
        callable_candidate = _boolean(
            item.get("callable_c_candidate"),
            f"SDK catalog symbol {symbol!r} callable_c_candidate",
        )
        signature = item.get("signature")
        if not callable_candidate:
            continue
        if signature is None:
            raise SdkCatalogError(
                f"SDK catalog marks {symbol!r} callable without a Clang signature"
            )
        signature = _mapping(signature, f"SDK catalog signature for {symbol!r}")
        if _string(signature.get("symbol"), f"signature symbol for {symbol!r}") != symbol:
            raise SdkCatalogError(f"SDK catalog/signature symbol mismatch for {symbol!r}")

        provider_rows = []
        seen_provider_rows: set[tuple[str, bool]] = set()
        for raw_fact in _list(
            item.get("sdk_direct_exports"),
            f"SDK catalog direct exports for {symbol!r}",
        ):
            fact = _mapping(raw_fact, f"SDK catalog direct export for {symbol!r}")
            if _string(fact.get("kind"), f"SDK catalog export kind for {symbol!r}") != "global":
                raise SdkCatalogError(
                    f"callable SDK catalog symbol {symbol!r} has non-global provider evidence"
                )
            install_name = _string(
                fact.get("install_name"),
                f"SDK catalog provider install_name for {symbol!r}",
            )
            weak = _boolean(fact.get("weak"), f"SDK catalog provider weak for {symbol!r}")
            key = (install_name, weak)
            if key in seen_provider_rows:
                continue
            seen_provider_rows.add(key)
            provider_rows.append({"install_name": install_name, "weak": weak})
        provider_rows.sort(key=lambda value: (value["install_name"], value["weak"]))

        projected.append(
            {
                "symbol": symbol,
                "required_by": [],
                "requirement_count": 0,
                "sdk_direct_exports": provider_rows,
                "signature": deepcopy(signature),
            }
        )

    projected.sort(key=lambda item: item["symbol"])
    return {
        "schema_version": 1,
        "kind": "typed-compatibility-inventory",
        "scope": "sdk-wide-mechanical-projection",
        "source_kind": "typed-sdk-catalog",
        "targets": {
            "clang": clang_target,
            "tapi": tapi_target,
        },
        "summary": {
            "requirement_count": 0,
            "unique_required_symbol_count": 0,
            "typed_unique_symbol_count": len(projected),
            "untyped_unique_symbol_count": 0,
            "sdk_wide_typed_symbol_count": len(projected),
        },
        "symbols": projected,
        "requirements": [],
    }


def _write_json(path: str | None, value: dict) -> None:
    rendered = json.dumps(value, indent=2, sort_keys=False) + "\n"
    if path:
        Path(path).write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
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
        help="write SDK-wide JSON catalog to this file instead of stdout",
    )
    parser.add_argument(
        "--abi-inventory-output",
        help=(
            "also write an SDK-wide mechanical projection accepted by abi_surface.py; "
            "this projection contains no Mach-O runtime requirements"
        ),
    )
    args = parser.parse_args(argv)

    try:
        catalog = build_sdk_catalog(
            load_manifest(Path(args.tapi), "TAPI"),
            load_manifest(Path(args.headers), "header"),
            tapi_target=args.tapi_target,
        )
        if args.output:
            _write_json(args.output, catalog)
        else:
            _write_json(None, catalog)
        if args.abi_inventory_output:
            _write_json(args.abi_inventory_output, build_abi_inventory(catalog))
        return 0
    except (SdkCatalogError, OSError) as exc:
        print(f"[sdk-catalog] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
