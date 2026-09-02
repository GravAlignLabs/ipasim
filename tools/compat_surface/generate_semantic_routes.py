#!/usr/bin/env python3
"""Generate ipaSim's explicit production semantic-route table.

The input is a machine-readable semantic-provider inventory. Only records whose
status is exactly ``approved`` are eligible for C++ route generation. SDK/TAPI,
Clang, ABI, and adapter metadata are intentionally not accepted as substitutes
for this approval source.

The generated table contains identities and policy only. Provider addresses are
still runtime facts and the loader must continue to verify the exact live module
and export address before selecting a route.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Sequence


class SemanticRouteError(ValueError):
    """Raised when semantic-provider approval data is unsafe or inconsistent."""


_ALLOWED_STATUSES = {
    "approved",
    "candidate",
    "missing",
    "complex",
    "unsupported",
}

# Keep this set synchronized with GeneratedSemanticImportRouter.cpp. The live
# profile names a runtime execution mechanism, not a handwritten ABI signature.
# GeneratedAdapterState means SysTranslator captures the live AAPCS64 state
# required by the selected generated AdapterRecord and lets that record drive
# argument capture, libffi invocation, and result commit.
_ALLOWED_LIVE_PROFILES = {
    "GeneratedAdapterState",
}

_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise SemanticRouteError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise SemanticRouteError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise SemanticRouteError(f"{label} must be a non-empty string")
    return value


def load_manifest(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise SemanticRouteError(
            f"could not read semantic provider inventory {path.name}: "
            f"{exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SemanticRouteError(
            f"semantic provider inventory {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, "semantic provider inventory")


def validate_manifest(manifest: dict) -> list[dict]:
    if manifest.get("schema_version") != 1:
        raise SemanticRouteError(
            "semantic provider inventory schema_version must be 1, got "
            f"{manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "semantic-provider-inventory":
        raise SemanticRouteError(
            "semantic provider inventory kind must be 'semantic-provider-inventory'"
        )

    providers = _list(manifest.get("providers"), "semantic providers")
    normalized = []
    seen_guest: set[str] = set()
    seen_adapter: set[str] = set()
    seen_provider_export: set[tuple[str, str]] = set()

    for index, raw in enumerate(providers):
        item = _mapping(raw, f"semantic providers[{index}]")
        guest_symbol = _string(
            item.get("guest_symbol"), f"semantic providers[{index}].guest_symbol"
        )
        status = _string(item.get("status"), f"semantic provider {guest_symbol!r} status")
        if status not in _ALLOWED_STATUSES:
            raise SemanticRouteError(
                f"semantic provider {guest_symbol!r} has unsupported status {status!r}"
            )
        evidence = _string(
            item.get("evidence"), f"semantic provider {guest_symbol!r} evidence"
        )
        if guest_symbol in seen_guest:
            raise SemanticRouteError(
                f"semantic provider inventory repeats guest symbol {guest_symbol!r}"
            )
        seen_guest.add(guest_symbol)

        normalized_item = {
            "guest_symbol": guest_symbol,
            "status": status,
            "evidence": evidence,
        }

        route_fields = (
            "host_export",
            "provider_module",
            "adapter_symbol",
            "semantic_owner",
            "live_profile",
        )
        if status == "approved":
            for field in route_fields:
                normalized_item[field] = _string(
                    item.get(field),
                    f"approved semantic provider {guest_symbol!r} {field}",
                )
            profile = normalized_item["live_profile"]
            if profile not in _ALLOWED_LIVE_PROFILES:
                raise SemanticRouteError(
                    f"approved semantic provider {guest_symbol!r} uses unsupported "
                    f"live profile {profile!r}"
                )
            if not _IDENTIFIER.fullmatch(profile):
                raise SemanticRouteError(
                    f"approved semantic provider {guest_symbol!r} has invalid "
                    f"live profile identifier {profile!r}"
                )

            adapter = normalized_item["adapter_symbol"]
            if adapter in seen_adapter:
                raise SemanticRouteError(
                    f"semantic provider inventory repeats approved adapter {adapter!r}"
                )
            seen_adapter.add(adapter)

            provider_key = (
                normalized_item["provider_module"].casefold(),
                normalized_item["host_export"],
            )
            if provider_key in seen_provider_export:
                raise SemanticRouteError(
                    "semantic provider inventory repeats approved provider export "
                    f"{normalized_item['provider_module']}!{normalized_item['host_export']}"
                )
            seen_provider_export.add(provider_key)
        else:
            # Non-approved rows are planning evidence only. Route-bearing fields
            # are forbidden so a status change cannot accidentally inherit stale
            # production-routing data.
            stale = [field for field in route_fields if field in item]
            if stale:
                raise SemanticRouteError(
                    f"non-approved semantic provider {guest_symbol!r} carries route fields: "
                    + ", ".join(sorted(stale))
                )

        normalized.append(normalized_item)

    return sorted(normalized, key=lambda item: item["guest_symbol"])


def approved_routes(manifest: dict) -> list[dict]:
    return [
        item
        for item in validate_manifest(manifest)
        if item["status"] == "approved"
    ]


def _cpp_string(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def _cpp_wide_string(value: str) -> str:
    return "L" + _cpp_string(value)


def render_cpp(manifest: dict) -> str:
    routes = approved_routes(manifest)
    lines = [
        "// Generated by tools/compat_surface/generate_semantic_routes.py from",
        "// tools/compat_surface/semantic_providers.json.",
        "//",
        "// Only explicitly approved semantic-provider records are emitted here.",
        "// SDK/compiler/ABI evidence alone never creates a production route.",
        "// Provider addresses remain runtime facts and are verified against the",
        "// exact live PE module/export before a route is selected.",
        "static constexpr ApprovedSemanticImportRoute ApprovedSemanticImportRoutes[] = {",
    ]
    for route in routes:
        lines.extend(
            [
                "    {",
                f"        {_cpp_string(route['guest_symbol'])},",
                f"        {_cpp_string(route['host_export'])},",
                f"        {_cpp_wide_string(route['provider_module'])},",
                f"        {_cpp_string(route['adapter_symbol'])},",
                f"        {_cpp_string(route['semantic_owner'])},",
                f"        LiveGuestProfile::{route['live_profile']},",
                "    },",
            ]
        )
    lines.extend(["};", ""])
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--providers", required=True)
    parser.add_argument("--output")
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if --output does not exactly match generated content",
    )
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest(Path(args.providers))
        rendered = render_cpp(manifest)
        if args.check:
            if not args.output:
                raise SemanticRouteError("--check requires --output")
            output_path = Path(args.output)
            try:
                existing = output_path.read_text(encoding="utf-8")
            except OSError as exc:
                raise SemanticRouteError(
                    f"could not read generated route table {output_path.name}: "
                    f"{exc.strerror or exc}"
                ) from exc
            if existing != rendered:
                raise SemanticRouteError(
                    f"generated semantic route table is stale: {output_path.name}"
                )
            return 0

        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (SemanticRouteError, OSError) as exc:
        print(f"[semantic-route-generator] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
