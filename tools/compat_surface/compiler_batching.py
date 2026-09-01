#!/usr/bin/env python3
"""Deterministic bounded orchestration for ipaSim's compiler-backed ABI probes.

The underlying AAPCS64 and Win64 modules remain the authoritative single-pass
lowering implementations. This module partitions their already-deterministic
symbol surfaces into bounded contiguous batches, invokes those proven
implementations, and merges the results back into the exact public schemas.

Batching is a scale mechanism only. It must not create semantic approval, change
ABI classifications, or turn an unsupported boundary into a generated candidate.
"""
from __future__ import annotations

import re
from copy import deepcopy
from typing import Callable

import abi_surface
import win64_abi_surface


DEFAULT_COMPILER_BATCH_SIZE = 256


class CompilerBatchError(ValueError):
    """Raised when a deterministic compiler batch cannot be lowered safely."""


def _validate_batch_size(batch_size: int) -> int:
    if isinstance(batch_size, bool) or not isinstance(batch_size, int):
        raise CompilerBatchError("compiler batch size must be an integer")
    if batch_size <= 0:
        raise CompilerBatchError("compiler batch size must be positive")
    return batch_size


def _chunks(items: list[dict], batch_size: int) -> list[list[dict]]:
    return [
        items[index : index + batch_size]
        for index in range(0, len(items), batch_size)
    ]


def _batch_label(kind: str, index: int, total: int, symbols: list[dict]) -> str:
    first = symbols[0]["symbol"]
    last = symbols[-1]["symbol"]
    return f"{kind} batch {index + 1}/{total} symbols {first!r}..{last!r}"


def _typed_inventory_symbols(inventory: dict) -> list[dict]:
    """Return raw typed rows in the authoritative AAPCS64 validator's order."""
    _, selected = abi_surface._validate_inventory(inventory)
    raw_symbols = inventory["symbols"]
    by_symbol = {
        item["symbol"]: item
        for item in raw_symbols
        if item.get("signature") is not None
    }
    return [deepcopy(by_symbol[item.symbol]) for item in selected]


def _aapcs64_summary(symbols: list[dict]) -> dict:
    return {
        "typed_symbol_count": len(symbols),
        "generated_bridge_candidate_count": sum(
            item.get("bridge_status") == "generated-bridge-candidate"
            for item in symbols
        ),
        "callback_runtime_count": sum(
            item.get("bridge_status") == "callback-runtime" for item in symbols
        ),
        "variadic_runtime_count": sum(
            item.get("bridge_status") == "variadic-runtime" for item in symbols
        ),
        "needs_manual_abi_count": sum(
            item.get("bridge_status") == "needs-manual-abi" for item in symbols
        ),
        "unsupported_no_prototype_count": sum(
            item.get("bridge_status") == "unsupported-no-prototype"
            for item in symbols
        ),
        "indirect_result_count": sum(
            item.get("return", {}).get("location", {}).get("kind")
            == "indirect-result"
            for item in symbols
        ),
        "indirect_aggregate_argument_count": sum(
            bool(parameter.get("indirect_source_aggregate"))
            for item in symbols
            for parameter in item.get("parameters", [])
            if isinstance(parameter, dict)
        ),
    }


def build_aapcs64_manifest(
    inventory: dict,
    *,
    batch_size: int = DEFAULT_COMPILER_BATCH_SIZE,
    **kwargs,
) -> dict:
    """Run ``abi_surface`` over deterministic bounded symbol batches."""
    batch_size = _validate_batch_size(batch_size)
    typed = _typed_inventory_symbols(inventory)
    if not typed or len(typed) <= batch_size:
        return abi_surface.build_abi_manifest(inventory, **kwargs)

    batches = _chunks(typed, batch_size)
    rendered: list[dict] = []
    failures: list[str] = []
    target = None
    for batch_index, batch in enumerate(batches):
        partial_inventory = deepcopy(inventory)
        partial_inventory["symbols"] = deepcopy(batch)
        try:
            partial = abi_surface.build_abi_manifest(partial_inventory, **kwargs)
        except abi_surface.AbiSurfaceError as exc:
            failures.append(
                f"{_batch_label('AAPCS64', batch_index, len(batches), batch)} "
                f"failed: {exc}"
            )
            continue
        if (
            partial.get("kind") != "aapcs64-abi-surface"
            or partial.get("schema_version") != 1
        ):
            raise CompilerBatchError(
                "AAPCS64 batch returned an unexpected manifest schema"
            )
        if target is None:
            target = partial.get("target")
        elif partial.get("target") != target:
            raise CompilerBatchError("AAPCS64 batches disagree on compiler target")
        symbols = partial.get("symbols")
        if not isinstance(symbols, list):
            raise CompilerBatchError("AAPCS64 batch symbols must be an array")
        rendered.extend(deepcopy(symbols))

    if failures:
        raise CompilerBatchError(
            f"AAPCS64 completed all {len(batches)} batches with "
            f"{len(failures)} failure(s):\n" + "\n".join(failures)
        )

    rendered.sort(key=lambda item: item["symbol"])
    return {
        "schema_version": 1,
        "kind": "aapcs64-abi-surface",
        "target": target,
        "summary": _aapcs64_summary(rendered),
        "symbols": rendered,
    }


_HOST_ADAPTER = re.compile(r"__ipasim_host_adapter_(\d{6})")
_CARRIER = re.compile(r"__ipasim_carrier_(\d{6})_s(\d+)_")
_CARRIER_ANY = re.compile(r"__ipasim_carrier_(\d{6})_")


def _rewrite_strings(value, rewrite: Callable[[str], str]):
    if isinstance(value, str):
        return rewrite(value)
    if isinstance(value, list):
        return [_rewrite_strings(item, rewrite) for item in value]
    if isinstance(value, dict):
        return {key: _rewrite_strings(item, rewrite) for key, item in value.items()}
    return value


def _win64_batch_synthetic_counts(symbols: list[dict]) -> tuple[int, int]:
    """Return the complete batch-local adapter/carrier namespace sizes.

    The public Win64 manifest only contains synthetic carrier names that survive
    Clang lowering into declarations or attributes. CarrierRenderer can also
    create intermediate/nested carrier typedefs whose names disappear from the
    emitted declaration surface. Those invisible names still consume numbering
    in a monolithic probe and therefore must count toward the next batch offset.
    """
    renderable: list[dict] = []
    for item in symbols:
        if item.get("bridge_status") != "generated-bridge-candidate":
            continue
        try:
            win64_abi_surface._build_probe([item])
        except win64_abi_surface.CarrierTypeError:
            continue
        renderable.append(item)

    if not renderable:
        return 0, 0

    source, helper_map, _ = win64_abi_surface._build_probe(renderable)
    if len(helper_map) != len(renderable):
        raise CompilerBatchError(
            "Win64 probe helper namespace does not match renderable symbols"
        )

    carrier_ids = sorted(
        {int(match.group(1)) for match in _CARRIER_ANY.finditer(source)}
    )
    if carrier_ids != list(range(len(carrier_ids))):
        raise CompilerBatchError(
            "Win64 generated carrier namespace is not dense"
        )
    return len(renderable), len(carrier_ids)


def _canonicalize_win64_batch(
    symbols: list[dict],
    *,
    adapter_offset: int,
    carrier_offset: int,
    expected_adapter_count: int,
    carrier_count: int,
) -> tuple[list[dict], int, int]:
    """Translate batch-local synthetic compiler names to monolithic numbering."""
    compiled = [
        item
        for item in symbols
        if isinstance(item.get("host_llvm_ir_name"), str)
    ]
    if len(compiled) != expected_adapter_count:
        raise CompilerBatchError(
            "Win64 batch compiled adapter count does not match generated probe"
        )

    adapter_map: dict[int, int] = {}
    for item in compiled:
        match = _HOST_ADAPTER.search(item["host_llvm_ir_name"])
        if match is None:
            raise CompilerBatchError(
                f"Win64 record {item.get('symbol')!r} has an unexpected "
                "synthetic adapter name"
            )
        local_index = int(match.group(1))
        if local_index in adapter_map:
            raise CompilerBatchError(
                f"Win64 batch repeats synthetic adapter index {local_index}"
            )
        adapter_map[local_index] = adapter_offset + local_index

    ordered_adapters = sorted(adapter_map)
    if ordered_adapters != list(range(expected_adapter_count)):
        raise CompilerBatchError("Win64 batch adapter numbering is not dense")

    carrier_refs: list[tuple[int, int]] = []

    def collect(value) -> None:
        if isinstance(value, str):
            carrier_refs.extend(
                (int(match.group(1)), int(match.group(2)))
                for match in _CARRIER.finditer(value)
            )
        elif isinstance(value, list):
            for item in value:
                collect(item)
        elif isinstance(value, dict):
            for item in value.values():
                collect(item)

    collect(symbols)
    for local_carrier, local_symbol in carrier_refs:
        if local_carrier < 0 or local_carrier >= carrier_count:
            raise CompilerBatchError(
                f"Win64 batch references carrier index {local_carrier} "
                f"outside generated namespace size {carrier_count}"
            )
        if local_symbol not in adapter_map:
            raise CompilerBatchError(
                f"Win64 batch carrier {local_carrier} references unknown "
                f"adapter index {local_symbol}"
            )

    def rewrite(text: str) -> str:
        def adapter_replacement(match: re.Match[str]) -> str:
            local = int(match.group(1))
            if local not in adapter_map:
                return match.group(0)
            return f"__ipasim_host_adapter_{adapter_map[local]:06d}"

        text = _HOST_ADAPTER.sub(adapter_replacement, text)

        def carrier_replacement(match: re.Match[str]) -> str:
            local_carrier = int(match.group(1))
            local_symbol = int(match.group(2))
            if local_carrier >= carrier_count or local_symbol not in adapter_map:
                return match.group(0)
            return (
                f"__ipasim_carrier_{carrier_offset + local_carrier:06d}_"
                f"s{adapter_map[local_symbol]}_"
            )

        return _CARRIER.sub(carrier_replacement, text)

    rewritten = _rewrite_strings(deepcopy(symbols), rewrite)
    return (
        rewritten,
        adapter_offset + expected_adapter_count,
        carrier_offset + carrier_count,
    )


def _win64_summary(symbols: list[dict]) -> dict:
    return {
        "guest_symbol_count": len(symbols),
        "cross_abi_adapter_candidate_count": sum(
            item.get("cross_abi_status") == "cross-abi-adapter-candidate"
            for item in symbols
        ),
        "inherited_runtime_boundary_count": sum(
            item.get("cross_abi_status") == "inherited-runtime-boundary"
            for item in symbols
        ),
        "needs_carrier_type_count": sum(
            item.get("cross_abi_status") == "needs-carrier-type"
            for item in symbols
        ),
        "needs_manual_host_abi_count": sum(
            item.get("cross_abi_status") == "needs-manual-host-abi"
            for item in symbols
        ),
    }


def build_win64_manifest(
    guest_manifest: dict,
    *,
    batch_size: int = DEFAULT_COMPILER_BATCH_SIZE,
    **kwargs,
) -> dict:
    """Run ``win64_abi_surface`` over deterministic bounded guest-symbol batches."""
    batch_size = _validate_batch_size(batch_size)
    _, validated_symbols = win64_abi_surface._validate_guest(guest_manifest)
    symbols = deepcopy(validated_symbols)
    if not symbols or len(symbols) <= batch_size:
        return win64_abi_surface.build_win64_manifest(guest_manifest, **kwargs)

    batches = _chunks(symbols, batch_size)
    rendered: list[dict] = []
    guest_target = None
    host_target = None
    adapter_offset = 0
    carrier_offset = 0
    for batch_index, batch in enumerate(batches):
        partial_guest = deepcopy(guest_manifest)
        partial_guest["symbols"] = deepcopy(batch)
        expected_adapter_count, carrier_count = _win64_batch_synthetic_counts(batch)
        try:
            partial = win64_abi_surface.build_win64_manifest(
                partial_guest, **kwargs
            )
        except (
            win64_abi_surface.Win64AbiError,
            win64_abi_surface.CarrierTypeError,
        ) as exc:
            raise CompilerBatchError(
                f"{_batch_label('Win64', batch_index, len(batches), batch)} "
                f"failed: {exc}"
            ) from exc
        if (
            partial.get("kind") != "win64-carrier-abi-surface"
            or partial.get("schema_version") != 1
        ):
            raise CompilerBatchError(
                "Win64 batch returned an unexpected manifest schema"
            )
        if guest_target is None:
            guest_target = partial.get("guest_target")
            host_target = partial.get("host_target")
        elif (
            partial.get("guest_target") != guest_target
            or partial.get("host_target") != host_target
        ):
            raise CompilerBatchError("Win64 batches disagree on compiler targets")
        partial_symbols = partial.get("symbols")
        if not isinstance(partial_symbols, list):
            raise CompilerBatchError("Win64 batch symbols must be an array")
        canonical, adapter_offset, carrier_offset = _canonicalize_win64_batch(
            partial_symbols,
            adapter_offset=adapter_offset,
            carrier_offset=carrier_offset,
            expected_adapter_count=expected_adapter_count,
            carrier_count=carrier_count,
        )
        rendered.extend(canonical)

    rendered.sort(key=lambda item: item["symbol"])
    return {
        "schema_version": 1,
        "kind": "win64-carrier-abi-surface",
        "guest_target": guest_target,
        "host_target": host_target,
        "summary": _win64_summary(rendered),
        "symbols": rendered,
    }
