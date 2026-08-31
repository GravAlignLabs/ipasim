#!/usr/bin/env python3
"""Compile libffi bridge-plan records into a deterministic runtime adapter table.

This layer consumes the machine-readable output of bridge_plan.py. It does not
bind Apple symbols to host functions and it does not infer semantic
compatibility. It only validates the mechanical guest capture/result-commit
operations that the runtime adapter can execute safely.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from copy import deepcopy
from pathlib import Path
from typing import Sequence


class RuntimeAdapterTableError(ValueError):
    """Raised when a bridge-plan record cannot be represented safely."""


_ALLOWED_BUILTINS = {
    "ffi_type_void",
    "ffi_type_uint8",
    "ffi_type_sint8",
    "ffi_type_uint16",
    "ffi_type_sint16",
    "ffi_type_uint32",
    "ffi_type_sint32",
    "ffi_type_uint64",
    "ffi_type_sint64",
    "ffi_type_float",
    "ffi_type_double",
    "ffi_type_pointer",
}

_CPP_TYPE_KINDS = {
    "ffi_type_void": "Void",
    "ffi_type_uint8": "UInt8",
    "ffi_type_sint8": "SInt8",
    "ffi_type_uint16": "UInt16",
    "ffi_type_sint16": "SInt16",
    "ffi_type_uint32": "UInt32",
    "ffi_type_sint32": "SInt32",
    "ffi_type_uint64": "UInt64",
    "ffi_type_sint64": "SInt64",
    "ffi_type_float": "Float",
    "ffi_type_double": "Double",
    "ffi_type_pointer": "Pointer",
}


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise RuntimeAdapterTableError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise RuntimeAdapterTableError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise RuntimeAdapterTableError(f"{label} must be a non-empty string")
    return value


def _integer(value, label: str, *, minimum: int | None = None) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise RuntimeAdapterTableError(f"{label} must be an integer")
    if minimum is not None and value < minimum:
        raise RuntimeAdapterTableError(f"{label} must be >= {minimum}")
    return value


def load_manifest(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeAdapterTableError(
            f"could not read bridge plan {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeAdapterTableError(
            f"bridge plan {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, "bridge plan")


def _normalize_type(raw, label: str) -> tuple[dict, str]:
    value = _mapping(raw, label)
    kind = _string(value.get("kind"), f"{label}.kind")
    if kind == "builtin":
        ffi_type = _string(value.get("ffi_type"), f"{label}.ffi_type")
        if ffi_type not in _ALLOWED_BUILTINS:
            raise RuntimeAdapterTableError(
                f"{label} uses unsupported runtime ffi type {ffi_type!r}"
            )
        pointer_shape = "top-level" if ffi_type == "ffi_type_pointer" else "none"
        normalized = {
            "kind": "builtin",
            "ffi_type": ffi_type,
        }
        if "width_bits" in value:
            normalized["width_bits"] = _integer(
                value["width_bits"], f"{label}.width_bits", minimum=1
            )
        if ffi_type == "ffi_type_pointer":
            policy = value.get("pointer_policy")
            if policy not in (
                None,
                "opaque-guest-address-requires-runtime-validation",
            ):
                raise RuntimeAdapterTableError(
                    f"{label} has unknown pointer policy {policy!r}"
                )
            normalized["pointer_policy"] = (
                "opaque-guest-address-requires-runtime-validation"
            )
        return normalized, pointer_shape

    if kind == "struct":
        elements = _list(value.get("elements"), f"{label}.elements")
        if not elements:
            raise RuntimeAdapterTableError(f"{label} has an empty struct carrier")
        rendered = []
        pointer_shapes = []
        for index, element in enumerate(elements):
            normalized, pointer_shape = _normalize_type(
                element, f"{label}.elements[{index}]"
            )
            rendered.append(normalized)
            pointer_shapes.append(pointer_shape)
        if any(shape != "none" for shape in pointer_shapes):
            pointer_shape = "nested"
        else:
            pointer_shape = "none"
        return {
            "kind": "struct",
            "elements": rendered,
            "layout": "libffi-computed",
        }, pointer_shape

    raise RuntimeAdapterTableError(f"{label} has unsupported kind {kind!r}")


def _parse_register(name: str, expected_bank: str, label: str) -> int:
    if expected_bank == "gpr":
        match = re.fullmatch(r"x([0-8])", name)
    elif expected_bank == "simd":
        match = re.fullmatch(r"v([0-7])", name)
    else:
        raise RuntimeAdapterTableError(
            f"{label} uses unsupported register bank {expected_bank!r}"
        )
    if not match:
        raise RuntimeAdapterTableError(
            f"{label} register {name!r} does not belong to {expected_bank}"
        )
    return int(match.group(1))


def _normalize_register_elements(raw: dict, label: str, operation: str) -> dict:
    location = _mapping(raw.get("guest_location"), f"{label}.guest_location")
    if location.get("kind") != "registers":
        raise RuntimeAdapterTableError(
            f"{label} {operation} requires guest_location.kind='registers'"
        )
    bank = _string(location.get("bank"), f"{label}.guest_location.bank")
    registers = _list(
        location.get("registers"), f"{label}.guest_location.registers"
    )
    if not registers:
        raise RuntimeAdapterTableError(f"{label} has no guest registers")
    indexes = [
        _parse_register(_string(name, f"{label}.register"), bank, label)
        for name in registers
    ]
    if len(set(indexes)) != len(indexes):
        raise RuntimeAdapterTableError(f"{label} repeats a guest register")
    element_count = _integer(
        raw.get("element_count"), f"{label}.element_count", minimum=1
    )
    if element_count != len(indexes):
        raise RuntimeAdapterTableError(
            f"{label}.element_count={element_count} does not match "
            f"{len(indexes)} guest register(s)"
        )
    width = _integer(
        raw.get("element_width_bytes"), f"{label}.element_width_bytes", minimum=1
    )
    if width > 16:
        raise RuntimeAdapterTableError(
            f"{label}.element_width_bytes exceeds a guest register lane"
        )
    if raw.get("byte_order") not in (None, "little-endian"):
        raise RuntimeAdapterTableError(
            f"{label} requires unsupported byte order {raw.get('byte_order')!r}"
        )
    if raw.get("register_lane") not in (None, "low-bytes"):
        raise RuntimeAdapterTableError(
            f"{label} requires unsupported register lane "
            f"{raw.get('register_lane')!r}"
        )
    return {
        "kind": "register-elements",
        "bank": bank,
        "registers": indexes,
        "element_width_bytes": width,
        "element_count": element_count,
    }


def _normalize_capture(raw, label: str) -> dict:
    value = _mapping(raw, label)
    operation = _string(value.get("operation"), f"{label}.operation")
    if operation == "capture-register-elements":
        return _normalize_register_elements(value, label, operation)
    if operation == "capture-result-pointer":
        location = _mapping(value.get("guest_location"), f"{label}.guest_location")
        if (
            location.get("kind") != "indirect-result"
            or location.get("register") != "x8"
        ):
            raise RuntimeAdapterTableError(
                f"{label} result-pointer capture must use ARM64 x8"
            )
        return {"kind": "result-pointer", "register": 8}
    if operation == "capture-guest-stack":
        offset = _integer(
            value.get("stack_offset"), f"{label}.stack_offset", minimum=0
        )
        location = _mapping(value.get("guest_location"), f"{label}.guest_location")
        if location.get("kind") != "stack":
            raise RuntimeAdapterTableError(
                f"{label} stack capture requires guest_location.kind='stack'"
            )
        return {"kind": "guest-stack", "stack_offset": offset}
    if operation.startswith("needs-"):
        raise RuntimeAdapterTableError(
            f"{label} still has unresolved operation {operation}"
        )
    raise RuntimeAdapterTableError(
        f"{label} uses unsupported capture operation {operation!r}"
    )


def _normalize_commit(raw, label: str) -> dict:
    value = _mapping(raw, label)
    operation = _string(value.get("operation"), f"{label}.operation")
    if operation == "none":
        return {"kind": "none"}
    if operation == "commit-register-elements":
        return _normalize_register_elements(value, label, operation)
    if operation == "commit-guest-stack":
        offset = _integer(
            value.get("stack_offset"), f"{label}.stack_offset", minimum=0
        )
        location = _mapping(value.get("guest_location"), f"{label}.guest_location")
        if location.get("kind") != "stack":
            raise RuntimeAdapterTableError(
                f"{label} stack commit requires guest_location.kind='stack'"
            )
        return {"kind": "guest-stack", "stack_offset": offset}
    if operation == "callee-writes-through-result-pointer":
        location = _mapping(value.get("guest_location"), f"{label}.guest_location")
        if (
            location.get("kind") != "indirect-result"
            or location.get("register") != "x8"
        ):
            raise RuntimeAdapterTableError(
                f"{label} indirect result commit must retain ARM64 x8"
            )
        return {"kind": "callee-writes-result-pointer", "register": 8}
    if operation.startswith("needs-"):
        raise RuntimeAdapterTableError(
            f"{label} still has unresolved operation {operation}"
        )
    raise RuntimeAdapterTableError(
        f"{label} uses unsupported commit operation {operation!r}"
    )


def _validate_root(manifest: dict) -> tuple[str, str, list[dict]]:
    if manifest.get("schema_version") != 1:
        raise RuntimeAdapterTableError(
            f"bridge plan schema_version must be 1, got "
            f"{manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "libffi-bridge-adapter-plan":
        raise RuntimeAdapterTableError(
            "bridge plan kind must be 'libffi-bridge-adapter-plan'"
        )
    guest_target = _string(manifest.get("guest_target"), "guest_target")
    host_target = _string(manifest.get("host_target"), "host_target")
    lower_guest = guest_target.lower()
    lower_host = host_target.lower()
    if not (
        lower_guest.startswith(("arm64-", "arm64e-"))
        and "-apple-ios" in lower_guest
    ):
        raise RuntimeAdapterTableError(
            f"guest target is not ARM64 iOS: {guest_target!r}"
        )
    if not (
        lower_host.startswith("x86_64-")
        and "windows" in lower_host
    ):
        raise RuntimeAdapterTableError(
            f"host target is not x86_64 Windows: {host_target!r}"
        )
    symbols = _list(manifest.get("symbols"), "bridge plan symbols")
    seen: set[str] = set()
    rendered = []
    for index, raw in enumerate(symbols):
        item = _mapping(raw, f"bridge plan symbols[{index}]")
        symbol = _string(item.get("symbol"), f"bridge plan symbols[{index}].symbol")
        if symbol in seen:
            raise RuntimeAdapterTableError(
                f"duplicate bridge-plan symbol {symbol!r}"
            )
        seen.add(symbol)
        _string(item.get("plan_status"), f"{symbol}.plan_status")
        rendered.append(item)
    return guest_target, host_target, sorted(
        rendered, key=lambda item: item["symbol"]
    )


def _normalize_adapter(item: dict) -> dict:
    symbol = item["symbol"]
    libffi = _mapping(item.get("libffi"), f"{symbol}.libffi")
    if libffi.get("abi") != "FFI_DEFAULT_ABI":
        raise RuntimeAdapterTableError(
            f"{symbol} requires unsupported libffi ABI {libffi.get('abi')!r}"
        )
    if libffi.get("prepare") not in (None, "ffi_prep_cif"):
        raise RuntimeAdapterTableError(
            f"{symbol} has unexpected prepare operation {libffi.get('prepare')!r}"
        )
    if libffi.get("call") not in (None, "ffi_call"):
        raise RuntimeAdapterTableError(
            f"{symbol} has unexpected call operation {libffi.get('call')!r}"
        )

    arguments = _list(item.get("arguments"), f"{symbol}.arguments")
    count = _integer(
        libffi.get("argument_count"), f"{symbol}.libffi.argument_count", minimum=0
    )
    if count != len(arguments):
        raise RuntimeAdapterTableError(
            f"{symbol} libffi argument_count={count} does not match "
            f"{len(arguments)} argument record(s)"
        )
    declared_types = _list(
        libffi.get("argument_types"), f"{symbol}.libffi.argument_types"
    )
    if len(declared_types) != len(arguments):
        raise RuntimeAdapterTableError(
            f"{symbol} libffi argument_types length does not match arguments"
        )

    normalized_arguments = []
    requires_pointer_validation = False
    for index, raw in enumerate(arguments):
        argument = _mapping(raw, f"{symbol}.arguments[{index}]")
        logical_index = _integer(
            argument.get("logical_index"),
            f"{symbol}.arguments[{index}].logical_index",
            minimum=0,
        )
        if logical_index != index:
            raise RuntimeAdapterTableError(
                f"{symbol} logical argument indexes must be dense and ordered"
            )
        arg_type, pointer_shape = _normalize_type(
            argument.get("ffi_type"), f"{symbol}.arguments[{index}].ffi_type"
        )
        declared_type, declared_pointer_shape = _normalize_type(
            declared_types[index],
            f"{symbol}.libffi.argument_types[{index}]",
        )
        if arg_type != declared_type or pointer_shape != declared_pointer_shape:
            raise RuntimeAdapterTableError(
                f"{symbol} argument {index} type disagrees with libffi descriptor"
            )
        if pointer_shape == "nested":
            raise RuntimeAdapterTableError(
                f"{symbol} argument {index} contains a nested pointer whose "
                "layout is not yet proven to the runtime"
            )
        requires_pointer_validation |= pointer_shape == "top-level"
        source_index = argument.get("source_index")
        if source_index is not None:
            source_index = _integer(
                source_index,
                f"{symbol}.arguments[{index}].source_index",
                minimum=0,
            )
        normalized_arguments.append(
            {
                "logical_index": logical_index,
                "kind": _string(
                    argument.get("kind"),
                    f"{symbol}.arguments[{index}].kind",
                ),
                "source_index": source_index,
                "type": arg_type,
                "capture": _normalize_capture(
                    argument.get("guest_capture"),
                    f"{symbol}.arguments[{index}].guest_capture",
                ),
                "indirect_source_aggregate": bool(
                    argument.get("indirect_source_aggregate")
                ),
            }
        )

    result = _mapping(item.get("result"), f"{symbol}.result")
    result_type, result_pointer_shape = _normalize_type(
        result.get("ffi_type"), f"{symbol}.result.ffi_type"
    )
    declared_result_type, declared_result_pointer_shape = _normalize_type(
        libffi.get("return_type"), f"{symbol}.libffi.return_type"
    )
    if (
        result_type != declared_result_type
        or result_pointer_shape != declared_result_pointer_shape
    ):
        raise RuntimeAdapterTableError(
            f"{symbol} result type disagrees with libffi descriptor"
        )
    if result_pointer_shape == "nested":
        raise RuntimeAdapterTableError(
            f"{symbol} result contains a nested pointer whose layout is not "
            "yet proven to the runtime"
        )
    requires_pointer_validation |= result_pointer_shape == "top-level"
    normalized_result = {
        "kind": _string(result.get("kind"), f"{symbol}.result.kind"),
        "type": result_type,
        "commit": _normalize_commit(
            result.get("guest_commit"), f"{symbol}.result.guest_commit"
        ),
    }

    return {
        "symbol": symbol,
        "source_plan_status": "libffi-descriptor-candidate",
        "execution_status": (
            "requires-pointer-validation"
            if requires_pointer_validation
            else "ready"
        ),
        "requires_pointer_validation": requires_pointer_validation,
        "arguments": normalized_arguments,
        "result": normalized_result,
    }


def build_runtime_adapter_table(manifest: dict) -> dict:
    guest_target, host_target, symbols = _validate_root(manifest)
    adapters = []
    boundaries = []
    for item in symbols:
        status = item["plan_status"]
        if status != "libffi-descriptor-candidate":
            boundaries.append(
                {
                    "symbol": item["symbol"],
                    "source_plan_status": status,
                    "reasons": deepcopy(item.get("reasons") or []),
                }
            )
            continue
        try:
            adapters.append(_normalize_adapter(item))
        except RuntimeAdapterTableError as exc:
            boundaries.append(
                {
                    "symbol": item["symbol"],
                    "source_plan_status": status,
                    "runtime_status": "needs-runtime-layout-or-policy",
                    "reasons": [str(exc)],
                }
            )

    adapters.sort(key=lambda item: item["symbol"])
    boundaries.sort(key=lambda item: item["symbol"])
    return {
        "schema_version": 1,
        "kind": "runtime-adapter-table",
        "source_kind": "libffi-bridge-adapter-plan",
        "guest_target": guest_target,
        "host_target": host_target,
        "execution_policy": {
            "binding": "explicit-host-implementation-only",
            "semantic_compatibility": "not-inferred",
            "pointer_policy": "validator-required-before-ffi-call-or-result-commit",
            "production_symbol_routing": False,
        },
        "summary": {
            "adapter_count": len(adapters),
            "ready_count": sum(
                item["execution_status"] == "ready" for item in adapters
            ),
            "pointer_validation_count": sum(
                item["requires_pointer_validation"] for item in adapters
            ),
            "boundary_count": len(boundaries),
        },
        "adapters": adapters,
        "boundaries": boundaries,
    }


def _cpp_type(type_spec: dict) -> str:
    if type_spec["kind"] == "builtin":
        return (
            "TypeSpec::builtin(ValueTypeKind::"
            + _CPP_TYPE_KINDS[type_spec["ffi_type"]]
            + ")"
        )
    return "TypeSpec::structure({" + ", ".join(
        _cpp_type(element) for element in type_spec["elements"]
    ) + "})"


def _cpp_bank(bank: str) -> str:
    return "GuestBank::Gpr" if bank == "gpr" else "GuestBank::Simd"


def _cpp_capture(capture: dict) -> str:
    kind = capture["kind"]
    if kind == "register-elements":
        registers = ", ".join(str(value) for value in capture["registers"])
        return (
            f"CaptureSpec::fromRegisters({_cpp_bank(capture['bank'])}, "
            f"{{{registers}}}, {capture['element_width_bytes']})"
        )
    if kind == "result-pointer":
        return "CaptureSpec::fromResultPointer()"
    if kind == "guest-stack":
        return f"CaptureSpec::fromStack({capture['stack_offset']})"
    raise RuntimeAdapterTableError(f"cannot render capture kind {kind!r}")


def _cpp_commit(commit: dict) -> str:
    kind = commit["kind"]
    if kind == "none":
        return "CommitSpec::none()"
    if kind == "register-elements":
        registers = ", ".join(str(value) for value in commit["registers"])
        return (
            f"CommitSpec::toRegisters({_cpp_bank(commit['bank'])}, "
            f"{{{registers}}}, {commit['element_width_bytes']})"
        )
    if kind == "guest-stack":
        return f"CommitSpec::toStack({commit['stack_offset']})"
    if kind == "callee-writes-result-pointer":
        return "CommitSpec::calleeWritesResultPointer()"
    raise RuntimeAdapterTableError(f"cannot render commit kind {kind!r}")


def _cpp_string(value: str) -> str:
    return json.dumps(value)


def render_cpp_table(
    table: dict,
    function_name: str = "makeGeneratedBridgeAdapterTable",
) -> str:
    if table.get("schema_version") != 1 or table.get("kind") != "runtime-adapter-table":
        raise RuntimeAdapterTableError(
            "C++ rendering requires runtime-adapter-table schema version 1"
        )
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", function_name):
        raise RuntimeAdapterTableError(
            f"invalid C++ function name {function_name!r}"
        )

    lines = [
        "// Generated by tools/compat_surface/runtime_adapter_table.py; do not edit.",
        f"static std::vector<ipasim::bridge::AdapterRecord> {function_name}() {{",
        "    using namespace ipasim::bridge;",
        "    return {",
    ]
    for adapter in table["adapters"]:
        lines.extend(
            [
                "        AdapterRecord{",
                f"            {_cpp_string(adapter['symbol'])},",
                (
                    "            true,"
                    if adapter["requires_pointer_validation"]
                    else "            false,"
                ),
                "            {",
            ]
        )
        for argument in adapter["arguments"]:
            source_index = (
                str(argument["source_index"])
                if argument["source_index"] is not None
                else "-1"
            )
            has_source = "true" if argument["source_index"] is not None else "false"
            indirect = (
                "true" if argument["indirect_source_aggregate"] else "false"
            )
            lines.extend(
                [
                    "                ArgumentSpec{",
                    f"                    {argument['logical_index']},",
                    f"                    {source_index},",
                    f"                    {has_source},",
                    f"                    {_cpp_type(argument['type'])},",
                    f"                    {_cpp_capture(argument['capture'])},",
                    f"                    {indirect}}},",
                ]
            )
        lines.extend(
            [
                "            },",
                "            ResultSpec{",
                f"                {_cpp_type(adapter['result']['type'])},",
                f"                {_cpp_commit(adapter['result']['commit'])}}}}},",
            ]
        )
    lines.extend(["    };", "}", ""])
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bridge-plan",
        required=True,
        help="libffi bridge adapter plan JSON from bridge_plan.py",
    )
    parser.add_argument("--output", help="runtime adapter table JSON")
    parser.add_argument("--cpp-output", help="generated C++ adapter table include")
    parser.add_argument(
        "--cpp-function-name",
        default="makeGeneratedBridgeAdapterTable",
    )
    args = parser.parse_args(argv)
    try:
        table = build_runtime_adapter_table(
            load_manifest(Path(args.bridge_plan))
        )
        rendered = json.dumps(table, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        elif not args.cpp_output:
            sys.stdout.write(rendered)
        if args.cpp_output:
            Path(args.cpp_output).write_text(
                render_cpp_table(table, args.cpp_function_name),
                encoding="utf-8",
            )
        return 0
    except (RuntimeAdapterTableError, OSError) as exc:
        print(f"[runtime-adapter-table] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
