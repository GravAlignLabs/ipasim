#!/usr/bin/env python3
"""Build an SDK-wide mechanical/semantic compatibility planning manifest.

This planner combines the SDK-wide typed catalog with optional AAPCS64 lowering
results and the explicit semantic-provider inventory. It is intentionally not a
runtime success predictor. Its job is to answer, in bulk:

* which SDK symbols are mechanically describable C-call candidates;
* which candidates have compiler-proven AAPCS64 lowering and what boundary class
  they are in;
* which symbols have an explicit semantic-provider status;
* which already-approved symbols are mechanically route-ready; and
* which providers/frameworks contain the largest remaining categories of work.

Unmentioned semantic providers are classified as ``unclassified``, not
``missing``. Absence from a small explicit approval manifest is not evidence that
an implementation does not exist elsewhere in ipaSim.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from copy import deepcopy
from pathlib import Path
from typing import Sequence

import generate_semantic_routes


class CompatibilityPlannerError(ValueError):
    """Raised when planning inputs are inconsistent or unsafe to join."""


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise CompatibilityPlannerError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise CompatibilityPlannerError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise CompatibilityPlannerError(f"{label} must be a non-empty string")
    return value


def load_manifest(path: Path, label: str) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CompatibilityPlannerError(
            f"could not read {label} manifest {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise CompatibilityPlannerError(
            f"{label} manifest {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, f"{label} manifest")


def _validate_catalog(manifest: dict) -> tuple[dict, list[dict]]:
    if manifest.get("schema_version") != 1:
        raise CompatibilityPlannerError(
            f"SDK catalog schema_version must be 1, got {manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "typed-sdk-catalog":
        raise CompatibilityPlannerError("SDK catalog kind must be 'typed-sdk-catalog'")
    targets = _mapping(manifest.get("targets"), "SDK catalog targets")
    clang_target = _string(targets.get("clang"), "SDK catalog Clang target")
    tapi_target = _string(targets.get("tapi"), "SDK catalog TAPI target")

    seen = set()
    symbols = []
    for index, raw in enumerate(_list(manifest.get("symbols"), "SDK catalog symbols")):
        item = _mapping(raw, f"SDK catalog symbols[{index}]")
        symbol = _string(item.get("symbol"), f"SDK catalog symbols[{index}].symbol")
        if symbol in seen:
            raise CompatibilityPlannerError(f"SDK catalog repeats symbol {symbol!r}")
        seen.add(symbol)
        classification = _string(
            item.get("classification"), f"SDK catalog symbol {symbol!r} classification"
        )
        callable_candidate = item.get("callable_c_candidate")
        if not isinstance(callable_candidate, bool):
            raise CompatibilityPlannerError(
                f"SDK catalog symbol {symbol!r} callable_c_candidate must be boolean"
            )
        exports = _list(
            item.get("sdk_direct_exports"),
            f"SDK catalog symbol {symbol!r} direct exports",
        )
        if not exports:
            raise CompatibilityPlannerError(
                f"SDK catalog symbol {symbol!r} has no target-matching direct export"
            )
        symbols.append(deepcopy(item))
    return {"clang": clang_target, "tapi": tapi_target}, symbols


def _validate_abi(manifest: dict | None, clang_target: str) -> dict[str, dict]:
    if manifest is None:
        return {}
    if manifest.get("schema_version") != 1:
        raise CompatibilityPlannerError(
            f"AAPCS64 manifest schema_version must be 1, got {manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "aapcs64-abi-surface":
        raise CompatibilityPlannerError(
            "AAPCS64 manifest kind must be 'aapcs64-abi-surface'"
        )
    if manifest.get("target") != clang_target:
        raise CompatibilityPlannerError(
            "SDK catalog and AAPCS64 targets differ: "
            f"{clang_target!r} vs {manifest.get('target')!r}"
        )
    result = {}
    for index, raw in enumerate(_list(manifest.get("symbols"), "AAPCS64 symbols")):
        item = _mapping(raw, f"AAPCS64 symbols[{index}]")
        symbol = _string(item.get("symbol"), f"AAPCS64 symbols[{index}].symbol")
        if symbol in result:
            raise CompatibilityPlannerError(f"AAPCS64 manifest repeats symbol {symbol!r}")
        _string(item.get("bridge_status"), f"AAPCS64 symbol {symbol!r} bridge_status")
        _list(item.get("bridge_reasons"), f"AAPCS64 symbol {symbol!r} bridge_reasons")
        result[symbol] = deepcopy(item)
    return result


def _semantic_index(manifest: dict) -> dict[str, dict]:
    try:
        normalized = generate_semantic_routes.validate_manifest(manifest)
    except generate_semantic_routes.SemanticRouteError as exc:
        raise CompatibilityPlannerError(str(exc)) from exc
    return {item["guest_symbol"]: deepcopy(item) for item in normalized}


def _mechanical_status(catalog_item: dict, abi_item: dict | None) -> tuple[str, list[str]]:
    if not catalog_item["callable_c_candidate"]:
        return (
            "not-callable-c",
            [f"SDK catalog classification is {catalog_item['classification']}"],
        )
    if abi_item is None:
        return (
            "typed-awaiting-aapcs64",
            ["exact Clang signature exists but no AAPCS64 lowering row was supplied"],
        )
    return abi_item["bridge_status"], list(abi_item.get("bridge_reasons", []))


def _route_status(
    callable_candidate: bool,
    mechanical_status: str,
    semantic_status: str,
) -> str:
    if semantic_status != "approved":
        return "not-approved"
    if not callable_candidate:
        return "approved-but-not-callable-c"
    if mechanical_status == "generated-bridge-candidate":
        return "approved-mechanical-route-ready"
    if mechanical_status == "typed-awaiting-aapcs64":
        return "approved-awaiting-aapcs64"
    return "approved-abi-boundary"


def _provider_names(item: dict) -> list[str]:
    names = set()
    for raw in item["sdk_direct_exports"]:
        fact = _mapping(raw, f"SDK direct export for {item['symbol']!r}")
        names.add(_string(fact.get("install_name"), "SDK direct export install_name"))
    return sorted(names)


def build_plan(
    catalog_manifest: dict,
    semantic_manifest: dict,
    *,
    abi_manifest: dict | None = None,
) -> dict:
    targets, catalog_symbols = _validate_catalog(catalog_manifest)
    abi = _validate_abi(abi_manifest, targets["clang"])
    semantics = _semantic_index(semantic_manifest)

    catalog_names = {item["symbol"] for item in catalog_symbols}
    orphan_abi = sorted(set(abi) - catalog_names)
    orphan_semantics = sorted(set(semantics) - catalog_names)

    rows = []
    mechanical_counts: Counter[str] = Counter()
    semantic_counts: Counter[str] = Counter()
    route_counts: Counter[str] = Counter()
    provider_stats: dict[str, Counter[str]] = defaultdict(Counter)

    for catalog_item in sorted(catalog_symbols, key=lambda item: item["symbol"]):
        symbol = catalog_item["symbol"]
        abi_item = abi.get(symbol)
        semantic_item = semantics.get(symbol)
        semantic_status = semantic_item["status"] if semantic_item else "unclassified"
        mechanical_status, mechanical_reasons = _mechanical_status(catalog_item, abi_item)
        route_status = _route_status(
            catalog_item["callable_c_candidate"],
            mechanical_status,
            semantic_status,
        )

        mechanical_counts[mechanical_status] += 1
        semantic_counts[semantic_status] += 1
        route_counts[route_status] += 1

        providers = _provider_names(catalog_item)
        for provider in providers:
            stats = provider_stats[provider]
            stats["symbol_count"] += 1
            if catalog_item["callable_c_candidate"]:
                stats["typed_c_candidate_count"] += 1
            stats[f"mechanical:{mechanical_status}"] += 1
            stats[f"semantic:{semantic_status}"] += 1
            stats[f"route:{route_status}"] += 1

        rows.append(
            {
                "symbol": symbol,
                "providers": providers,
                "sdk_classification": catalog_item["classification"],
                "callable_c_candidate": catalog_item["callable_c_candidate"],
                "mechanical_status": mechanical_status,
                "mechanical_reasons": mechanical_reasons,
                "semantic_status": semantic_status,
                "semantic_provider": deepcopy(semantic_item) if semantic_item else None,
                "route_status": route_status,
            }
        )

    provider_summary = []
    for provider in sorted(provider_stats):
        stats = provider_stats[provider]
        provider_summary.append(
            {
                "install_name": provider,
                "symbol_count": stats["symbol_count"],
                "typed_c_candidate_count": stats["typed_c_candidate_count"],
                "generated_bridge_candidate_count": stats[
                    "mechanical:generated-bridge-candidate"
                ],
                "typed_awaiting_aapcs64_count": stats[
                    "mechanical:typed-awaiting-aapcs64"
                ],
                "callback_runtime_count": stats["mechanical:callback-runtime"],
                "variadic_runtime_count": stats["mechanical:variadic-runtime"],
                "needs_manual_abi_count": stats["mechanical:needs-manual-abi"],
                "unsupported_no_prototype_count": stats[
                    "mechanical:unsupported-no-prototype"
                ],
                "semantic_approved_count": stats["semantic:approved"],
                "semantic_candidate_count": stats["semantic:candidate"],
                "semantic_missing_count": stats["semantic:missing"],
                "semantic_complex_count": stats["semantic:complex"],
                "semantic_unsupported_count": stats["semantic:unsupported"],
                "semantic_unclassified_count": stats["semantic:unclassified"],
                "approved_mechanical_route_ready_count": stats[
                    "route:approved-mechanical-route-ready"
                ],
            }
        )

    return {
        "schema_version": 1,
        "kind": "sdk-compatibility-plan",
        "targets": targets,
        "summary": {
            "symbol_count": len(rows),
            "typed_c_candidate_count": sum(
                1 for row in rows if row["callable_c_candidate"]
            ),
            "mechanical_status_counts": dict(sorted(mechanical_counts.items())),
            "semantic_status_counts": dict(sorted(semantic_counts.items())),
            "route_status_counts": dict(sorted(route_counts.items())),
            "provider_count": len(provider_summary),
            "orphan_aapcs64_symbol_count": len(orphan_abi),
            "orphan_semantic_provider_count": len(orphan_semantics),
        },
        "provider_summary": provider_summary,
        "symbols": rows,
        "orphan_aapcs64_symbols": orphan_abi,
        "orphan_semantic_providers": orphan_semantics,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--semantic-providers", required=True)
    parser.add_argument(
        "--aapcs64",
        help="optional AAPCS64 ABI surface; without it typed rows remain awaiting lowering",
    )
    parser.add_argument("--output")
    args = parser.parse_args(argv)

    try:
        plan = build_plan(
            load_manifest(Path(args.catalog), "SDK catalog"),
            load_manifest(Path(args.semantic_providers), "semantic provider"),
            abi_manifest=(
                load_manifest(Path(args.aapcs64), "AAPCS64") if args.aapcs64 else None
            ),
        )
        rendered = json.dumps(plan, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (CompatibilityPlannerError, OSError) as exc:
        print(f"[compat-planner] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
