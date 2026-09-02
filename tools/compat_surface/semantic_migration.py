#!/usr/bin/env python3
"""Join existing host exports to generated SDK adapters as migration candidates.

This module identifies places where ipaSim already exposes a callable Windows
host export and the SDK-wide compatibility engine already generated a matching
runtime adapter. That combination is useful migration evidence, but it is not
semantic approval. Only the separate semantic-provider inventory can approve a
production route.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from copy import deepcopy
from pathlib import Path
from typing import Sequence

import generate_semantic_routes


class SemanticMigrationError(ValueError):
    """Raised when migration-planning inputs are inconsistent."""


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise SemanticMigrationError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise SemanticMigrationError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise SemanticMigrationError(f"{label} must be a non-empty string")
    return value


def _validate_host_exports(manifest: dict) -> tuple[str, list[dict]]:
    if manifest.get("schema_version") != 1:
        raise SemanticMigrationError(
            "host export surface schema_version must be 1, got "
            f"{manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "pe-def-export-surface":
        raise SemanticMigrationError(
            "host export surface kind must be 'pe-def-export-surface'"
        )
    module = _string(manifest.get("library_filename"), "host export library_filename")
    seen: set[str] = set()
    exports: list[dict] = []
    for index, raw in enumerate(_list(manifest.get("exports"), "host exports")):
        item = _mapping(raw, f"host exports[{index}]")
        name = _string(item.get("name"), f"host exports[{index}].name")
        target = _string(item.get("target"), f"host export {name!r} target")
        kind = _string(item.get("kind"), f"host export {name!r} kind")
        if kind not in {"function", "data"}:
            raise SemanticMigrationError(
                f"host export {name!r} has unsupported kind {kind!r}"
            )
        if name in seen:
            raise SemanticMigrationError(f"host export surface repeats {name!r}")
        seen.add(name)
        exports.append({"name": name, "target": target, "kind": kind})
    return module, sorted(exports, key=lambda item: item["name"])


def _runtime_adapter_symbols(manifest: dict) -> set[str]:
    if manifest.get("schema_version") != 1:
        raise SemanticMigrationError(
            "runtime adapter table schema_version must be 1, got "
            f"{manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "runtime-adapter-table":
        raise SemanticMigrationError(
            "runtime adapter table kind must be 'runtime-adapter-table'"
        )
    result: set[str] = set()
    for index, raw in enumerate(_list(manifest.get("adapters"), "runtime adapters")):
        item = _mapping(raw, f"runtime adapters[{index}]")
        symbol = _string(item.get("symbol"), f"runtime adapters[{index}].symbol")
        if symbol in result:
            raise SemanticMigrationError(f"runtime adapter table repeats {symbol!r}")
        result.add(symbol)
    return result


def _semantic_index(manifest: dict) -> dict[str, dict]:
    try:
        normalized = generate_semantic_routes.validate_manifest(manifest)
    except generate_semantic_routes.SemanticRouteError as exc:
        raise SemanticMigrationError(str(exc)) from exc
    return {item["guest_symbol"]: deepcopy(item) for item in normalized}


def _validate_approved_identity(
    semantic: dict,
    *,
    guest_symbol: str,
    host_export: str,
    provider_module: str,
) -> None:
    if semantic["adapter_symbol"] != guest_symbol:
        raise SemanticMigrationError(
            f"approved semantic provider {guest_symbol!r} adapter identity differs from generated guest symbol"
        )
    if semantic["host_export"] != host_export:
        raise SemanticMigrationError(
            f"approved semantic provider {guest_symbol!r} host export differs from PE .def export"
        )
    if semantic["provider_module"].casefold() != provider_module.casefold():
        raise SemanticMigrationError(
            f"approved semantic provider {guest_symbol!r} module differs from PE .def library"
        )


def build_migration_plan(
    host_export_manifest: dict,
    runtime_adapter_manifest: dict,
    semantic_manifest: dict,
) -> dict:
    provider_module, exports = _validate_host_exports(host_export_manifest)
    adapters = _runtime_adapter_symbols(runtime_adapter_manifest)
    semantics = _semantic_index(semantic_manifest)

    rows: list[dict] = []
    counts: Counter[str] = Counter()
    candidate_symbols: list[str] = []

    for export in exports:
        host_export = export["name"]
        guest_symbol = "_" + host_export
        semantic = semantics.get(guest_symbol)
        semantic_status = semantic["status"] if semantic else "unclassified"
        adapter_available = guest_symbol in adapters

        if export["kind"] == "data":
            migration_status = "data-export"
            reasons = ["PE .def marks this export as data, so it is not callable"]
        elif not adapter_available:
            migration_status = "no-generated-adapter"
            reasons = ["host function export exists but no SDK-wide runtime adapter was generated"]
        elif semantic_status == "approved":
            _validate_approved_identity(
                semantic,
                guest_symbol=guest_symbol,
                host_export=host_export,
                provider_module=provider_module,
            )
            migration_status = "already-approved"
            reasons = ["explicit semantic-provider inventory already approves this exact route"]
        elif semantic_status in {"missing", "complex", "unsupported"}:
            migration_status = f"semantic-{semantic_status}"
            reasons = [
                f"explicit semantic-provider inventory classifies this symbol as {semantic_status}"
            ]
        else:
            migration_status = "migration-candidate"
            reasons = [
                "callable PE host export and SDK-wide generated runtime adapter both exist",
                "semantic approval is still required before production routing",
            ]
            candidate_symbols.append(guest_symbol)

        counts[migration_status] += 1
        rows.append(
            {
                "guest_symbol": guest_symbol,
                "host_export": host_export,
                "host_target": export["target"],
                "provider_module": provider_module,
                "host_export_kind": export["kind"],
                "generated_adapter_available": adapter_available,
                "semantic_status": semantic_status,
                "semantic_provider": deepcopy(semantic) if semantic else None,
                "migration_status": migration_status,
                "reasons": reasons,
            }
        )

    rows.sort(key=lambda item: item["guest_symbol"])
    candidate_symbols.sort()
    return {
        "schema_version": 1,
        "kind": "semantic-migration-plan",
        "provider_module": provider_module,
        "summary": {
            "host_export_count": len(rows),
            "function_export_count": sum(row["host_export_kind"] == "function" for row in rows),
            "data_export_count": sum(row["host_export_kind"] == "data" for row in rows),
            "generated_adapter_match_count": sum(row["generated_adapter_available"] for row in rows),
            "migration_candidate_count": counts["migration-candidate"],
            "already_approved_count": counts["already-approved"],
            "status_counts": dict(sorted(counts.items())),
        },
        "candidate_symbols": candidate_symbols,
        "exports": rows,
    }


def load_json(path: Path, label: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise SemanticMigrationError(
            f"could not read {label} {path.name}: {exc.strerror or exc}"
        ) from exc
    except json.JSONDecodeError as exc:
        raise SemanticMigrationError(
            f"{label} {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, label)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-exports", required=True)
    parser.add_argument("--runtime-adapters", required=True)
    parser.add_argument("--semantic-providers", required=True)
    parser.add_argument("--output")
    args = parser.parse_args(argv)

    try:
        plan = build_migration_plan(
            load_json(Path(args.host_exports), "host export surface"),
            load_json(Path(args.runtime_adapters), "runtime adapter table"),
            load_json(Path(args.semantic_providers), "semantic provider inventory"),
        )
        rendered = json.dumps(plan, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (SemanticMigrationError, OSError) as exc:
        print(f"[semantic-migration] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
