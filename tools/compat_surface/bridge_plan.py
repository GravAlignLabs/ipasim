#!/usr/bin/env python3
"""Build a deterministic libffi-oriented bridge adapter plan for ipaSim.

Consumes the compiler-backed Win64 carrier ABI surface. The result describes the
mechanical capture, libffi carrier types, temporary storage, and guest result
commit operations required for cross-ABI adapter candidates.

This is planning evidence only. It does not call a host implementation, validate
guest pointers, infer API semantics, or generate executable code.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from copy import deepcopy
from pathlib import Path
from typing import Sequence


class BridgePlanError(ValueError):
    """Raised when a cross-ABI record cannot be converted safely."""


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise BridgePlanError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise BridgePlanError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise BridgePlanError(f"{label} must be a non-empty string")
    return value


def load_manifest(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise BridgePlanError(
            f"could not read Win64 ABI manifest {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise BridgePlanError(
            f"Win64 ABI manifest {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, "Win64 ABI manifest")


def _validate_manifest(manifest: dict) -> tuple[str, str, list[dict]]:
    if manifest.get("schema_version") != 1:
        raise BridgePlanError(
            f"Win64 ABI schema_version must be 1, got {manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "win64-carrier-abi-surface":
        raise BridgePlanError(
            "Win64 ABI kind must be 'win64-carrier-abi-surface'"
        )
    guest_target = _string(manifest.get("guest_target"), "guest target")
    host_target = _string(manifest.get("host_target"), "host target")
    if not (
        guest_target.lower().startswith(("arm64-", "arm64e-"))
        and "-apple-ios" in guest_target.lower()
    ):
        raise BridgePlanError(f"guest target is not ARM64 iOS: {guest_target!r}")
    if not (
        host_target.lower().startswith("x86_64-")
        and "windows" in host_target.lower()
    ):
        raise BridgePlanError(f"host target is not x86_64 Windows: {host_target!r}")
    symbols = _list(manifest.get("symbols"), "Win64 ABI symbols")
    seen: set[str] = set()
    rendered = []
    for index, raw in enumerate(symbols):
        item = _mapping(raw, f"Win64 ABI symbols[{index}]")
        symbol = _string(item.get("symbol"), f"Win64 ABI symbols[{index}].symbol")
        if symbol in seen:
            raise BridgePlanError(f"duplicate Win64 ABI symbol {symbol!r}")
        seen.add(symbol)
        _string(item.get("cross_abi_status"), f"cross_abi_status for {symbol}")
        rendered.append(item)
    return guest_target, host_target, sorted(rendered, key=lambda item: item["symbol"])


def _split_top_level(text: str, delimiter: str = ",") -> list[str]:
    parts: list[str] = []
    start = 0
    stack: list[str] = []
    pairs = {")": "(", "]": "[", "}": "{", ">": "<"}
    in_quote = False
    escaped = False
    for index, char in enumerate(text):
        if in_quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_quote = False
            continue
        if char == '"':
            in_quote = True
            continue
        if char in "([{<":
            stack.append(char)
        elif char in ")]}>" and stack and stack[-1] == pairs[char]:
            stack.pop()
        elif char == delimiter and not stack:
            parts.append(text[start:index].strip())
            start = index + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def _ffi_integer(bits: int, attrs: Sequence[str]) -> dict:
    if bits not in (1, 8, 16, 32, 64):
        raise BridgePlanError(f"libffi carrier does not support i{bits}")
    if bits == 1:
        return {
            "kind": "builtin",
            "ffi_type": "ffi_type_uint8",
            "width_bits": 1,
            "integer_semantics": "boolean-low-bit",
        }
    attrs_set = set(attrs)
    if bits in (8, 16) and not ({"signext", "zeroext"} & attrs_set):
        raise BridgePlanError(
            f"i{bits} carrier has no signext/zeroext evidence for Win64"
        )
    signed = "signext" in attrs_set
    prefix = "sint" if signed else "uint"
    return {
        "kind": "builtin",
        "ffi_type": f"ffi_type_{prefix}{bits}",
        "width_bits": bits,
        "integer_semantics": "signed" if signed else "raw-bits",
    }


def _ffi_type(ir_type: str, attrs: Sequence[str] = ()) -> dict:
    value = ir_type.strip()
    if value == "void":
        return {"kind": "builtin", "ffi_type": "ffi_type_void"}
    if value == "ptr":
        return {
            "kind": "builtin",
            "ffi_type": "ffi_type_pointer",
            "width_bits": 64,
            "pointer_policy": "opaque-guest-address-requires-runtime-validation",
        }
    integer = re.fullmatch(r"i(\d+)", value)
    if integer:
        return _ffi_integer(int(integer.group(1)), attrs)
    floating = {
        "float": ("ffi_type_float", 32),
        "double": ("ffi_type_double", 64),
    }.get(value)
    if floating:
        return {
            "kind": "builtin",
            "ffi_type": floating[0],
            "width_bits": floating[1],
        }
    if value in {"half", "bfloat", "fp128"}:
        raise BridgePlanError(
            f"vendored libffi 3.2.1 has no proven carrier for {value}"
        )
    array = re.fullmatch(r"\[\s*(\d+)\s+x\s+(.+)\]", value)
    if array:
        count = int(array.group(1))
        element = _ffi_type(array.group(2).strip())
        return {
            "kind": "struct",
            "source_kind": "llvm-array",
            "count": count,
            "elements": [deepcopy(element) for _ in range(count)],
            "layout": "libffi-computed",
        }
    if value.startswith("{") and value.endswith("}"):
        fields = _split_top_level(value[1:-1])
        return {
            "kind": "struct",
            "source_kind": "llvm-struct",
            "elements": [_ffi_type(field) for field in fields],
            "layout": "libffi-computed",
        }
    if value.startswith("<"):
        raise BridgePlanError(
            f"LLVM vector/packed carrier {value} is not proven for vendored libffi"
        )
    if value.startswith("%"):
        raise BridgePlanError(
            f"named LLVM carrier {value} has no preserved layout"
        )
    raise BridgePlanError(f"unsupported libffi carrier type {value!r}")


def _scalar_width_bytes(ir_type: str) -> int | None:
    value = ir_type.strip()
    integer = re.fullmatch(r"i(\d+)", value)
    if integer:
        bits = int(integer.group(1))
        return (bits + 7) // 8 if bits <= 64 else None
    return {
        "ptr": 8,
        "float": 4,
        "double": 8,
    }.get(value)


def _element_codec(ir_type: str) -> tuple[int, int] | None:
    """Return (element_count, element_width_bytes) for safely splittable carriers."""
    scalar = _scalar_width_bytes(ir_type)
    if scalar is not None:
        return (1, scalar)
    array = re.fullmatch(r"\[\s*(\d+)\s+x\s+(.+)\]", ir_type.strip())
    if array:
        count = int(array.group(1))
        width = _scalar_width_bytes(array.group(2).strip())
        if width is not None:
            return count, width
    return None


def _guest_capture(location: dict, ir_type: str) -> dict:
    kind = location.get("kind")
    codec = _element_codec(ir_type)
    if kind == "registers":
        registers = _list(location.get("registers"), "guest registers")
        if codec is None:
            return {
                "operation": "needs-capture-layout",
                "guest_location": deepcopy(location),
                "reason": f"carrier {ir_type} is not safely splittable across guest registers",
            }
        count, width = codec
        if len(registers) != count:
            return {
                "operation": "needs-capture-layout",
                "guest_location": deepcopy(location),
                "reason": (
                    f"carrier {ir_type} has {count} logical element(s) but "
                    f"{len(registers)} guest register(s)"
                ),
            }
        return {
            "operation": "capture-register-elements",
            "guest_location": deepcopy(location),
            "element_width_bytes": width,
            "element_count": count,
            "byte_order": "little-endian",
            "register_lane": "low-bytes",
        }
    if kind == "indirect-result":
        return {
            "operation": "capture-result-pointer",
            "guest_location": deepcopy(location),
            "element_width_bytes": 8,
        }
    if kind == "stack":
        offset = location.get("stack_offset")
        if offset is None:
            return {
                "operation": "needs-guest-stack-layout",
                "guest_location": deepcopy(location),
            }
        return {
            "operation": "capture-guest-stack",
            "guest_location": deepcopy(location),
            "stack_offset": offset,
        }
    return {
        "operation": "needs-capture-layout",
        "guest_location": deepcopy(location),
        "reason": f"unknown guest location kind {kind!r}",
    }


def _guest_commit(location: dict, ir_type: str) -> dict:
    kind = location.get("kind")
    if kind == "void":
        return {"operation": "none"}
    capture = _guest_capture(location, ir_type)
    operation = capture.pop("operation")
    if operation == "capture-register-elements":
        capture["operation"] = "commit-register-elements"
        return capture
    if operation == "capture-guest-stack":
        capture["operation"] = "commit-guest-stack"
        return capture
    if operation.startswith("needs-"):
        return {"operation": operation, **capture}
    return {
        "operation": "needs-result-layout",
        "guest_location": deepcopy(location),
    }


def _host_evidence(transfer: dict) -> dict:
    return {
        "lowered_ir_type": transfer.get("host_lowered_ir_type"),
        "llvm_attributes": deepcopy(transfer.get("host_llvm_attributes") or []),
        "location": deepcopy(transfer.get("host_location") or {}),
        "transfer_complexity": transfer.get("transfer_complexity"),
    }


def _plan_argument(transfer: dict, logical_index: int) -> dict:
    kind = _string(transfer.get("kind"), f"argument {logical_index} kind")
    ir_type = _string(
        transfer.get("carrier_ir_type"), f"argument {logical_index} carrier_ir_type"
    )
    guest_location = _mapping(
        transfer.get("guest_location"), f"argument {logical_index} guest_location"
    )
    attrs = _list(
        transfer.get("host_llvm_attributes") or [],
        f"argument {logical_index} host_llvm_attributes",
    )
    descriptor = _ffi_type(ir_type, attrs)
    capture = _guest_capture(guest_location, ir_type)
    return {
        "logical_index": logical_index,
        "kind": kind,
        "source_index": transfer.get("source_index"),
        "carrier_ir_type": ir_type,
        "ffi_type": descriptor,
        "storage": {
            "kind": "ffi-argument-storage",
            "size": "from-ffi_type-after-ffi_prep_cif",
        },
        "guest_capture": capture,
        "host_abi_evidence": _host_evidence(transfer),
        "indirect_source_aggregate": bool(
            transfer.get("indirect_source_aggregate")
        ),
    }


def _normal_result(result: dict) -> dict:
    ir_type = _string(result.get("carrier_ir_type"), "result carrier_ir_type")
    guest_location = _mapping(result.get("guest_location"), "result guest_location")
    host_result = _mapping(
        result.get("host_carrier_result"), "result host_carrier_result"
    )
    descriptor = _ffi_type(ir_type)
    return {
        "kind": "value-result",
        "carrier_ir_type": ir_type,
        "ffi_type": descriptor,
        "storage": {
            "kind": "ffi-result-storage",
            "size": "from-ffi_type-after-ffi_prep_cif",
        },
        "guest_commit": _guest_commit(guest_location, ir_type),
        "host_abi_evidence": {
            "result": deepcopy(host_result),
            "transfer_complexity": result.get("transfer_complexity"),
        },
    }


def _indirect_result(result: dict) -> tuple[dict, dict]:
    transfer = _mapping(
        result.get("result_pointer_transfer"), "result_pointer_transfer"
    )
    # PR #44 made the guest x8 result pointer an explicit first carrier argument.
    argument = _plan_argument(transfer, 0)
    return (
        argument,
        {
            "kind": "guest-indirect-result-pointer",
            "ffi_type": {"kind": "builtin", "ffi_type": "ffi_type_void"},
            "storage": {"kind": "none"},
            "guest_commit": {
                "operation": "callee-writes-through-result-pointer",
                "guest_location": deepcopy(result.get("guest_location") or {}),
            },
            "host_abi_evidence": {
                "result": deepcopy(result.get("host_carrier_result") or {}),
            },
        },
    )


def _plan_symbol(item: dict) -> dict:
    symbol = _string(item.get("symbol"), "symbol")
    source_status = _string(item.get("cross_abi_status"), f"{symbol} cross_abi_status")
    if source_status != "cross-abi-adapter-candidate":
        return {
            "symbol": symbol,
            "source_cross_abi_status": source_status,
            "plan_status": "inherited-boundary",
            "reasons": deepcopy(item.get("reasons") or []),
            "arguments": [],
            "result": None,
        }

    parameters = _list(item.get("parameters"), f"{symbol} parameters")
    result = _mapping(item.get("return"), f"{symbol} return")
    arguments: list[dict] = []
    logical_index = 0

    try:
        if result.get("kind") == "guest-indirect-result-pointer":
            result_argument, result_plan = _indirect_result(result)
            arguments.append(result_argument)
            logical_index += 1
        elif result.get("kind") == "value-result":
            result_plan = _normal_result(result)
        else:
            raise BridgePlanError(
                f"{symbol}: unsupported return transfer kind {result.get('kind')!r}"
            )

        for raw in parameters:
            transfer = _mapping(raw, f"{symbol} parameter transfer")
            arguments.append(_plan_argument(transfer, logical_index))
            logical_index += 1
    except BridgePlanError as exc:
        return {
            "symbol": symbol,
            "source_cross_abi_status": source_status,
            "plan_status": "needs-ffi-type-or-layout",
            "reasons": [str(exc)],
            "arguments": [],
            "result": None,
        }

    blockers: list[str] = []
    for argument in arguments:
        operation = argument["guest_capture"].get("operation")
        if isinstance(operation, str) and operation.startswith("needs-"):
            blockers.append(
                f"argument {argument['logical_index']}: {operation}"
            )
    commit = result_plan["guest_commit"].get("operation")
    if isinstance(commit, str) and commit.startswith("needs-"):
        blockers.append(f"result: {commit}")

    status = "libffi-descriptor-candidate" if not blockers else "needs-guest-layout"
    return {
        "symbol": symbol,
        "source_cross_abi_status": source_status,
        "plan_status": status,
        "reasons": blockers,
        "libffi": {
            "abi": "FFI_DEFAULT_ABI",
            "prepare": "ffi_prep_cif",
            "call": "ffi_call",
            "argument_count": len(arguments),
            "argument_types": [deepcopy(arg["ffi_type"]) for arg in arguments],
            "return_type": deepcopy(result_plan["ffi_type"]),
        },
        "arguments": arguments,
        "result": result_plan,
    }


def build_bridge_plan(manifest: dict) -> dict:
    guest_target, host_target, symbols = _validate_manifest(manifest)
    planned = [_plan_symbol(item) for item in symbols]
    planned.sort(key=lambda item: item["symbol"])
    return {
        "schema_version": 1,
        "kind": "libffi-bridge-adapter-plan",
        "guest_target": guest_target,
        "host_target": host_target,
        "libffi": {
            "repository_path": "deps/Libffi",
            "observed_version": "3.2.1",
            "observed_backend": "X86_WIN64",
            "api": ["ffi_prep_cif", "ffi_call"],
            "closures_available_in_vendored_port": True,
            "note": (
                "descriptor evidence only; pointer validity and semantic host "
                "implementation ownership remain runtime responsibilities"
            ),
        },
        "summary": {
            "symbol_count": len(planned),
            "libffi_descriptor_candidate_count": sum(
                item["plan_status"] == "libffi-descriptor-candidate"
                for item in planned
            ),
            "needs_guest_layout_count": sum(
                item["plan_status"] == "needs-guest-layout"
                for item in planned
            ),
            "needs_ffi_type_or_layout_count": sum(
                item["plan_status"] == "needs-ffi-type-or-layout"
                for item in planned
            ),
            "inherited_boundary_count": sum(
                item["plan_status"] == "inherited-boundary"
                for item in planned
            ),
        },
        "symbols": planned,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--win64-abi",
        required=True,
        help="Win64 carrier ABI surface JSON from win64_abi_surface.py",
    )
    parser.add_argument("--output")
    args = parser.parse_args(argv)
    try:
        plan = build_bridge_plan(load_manifest(Path(args.win64_abi)))
        rendered = json.dumps(plan, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (BridgePlanError, OSError) as exc:
        print(f"[bridge-plan] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
