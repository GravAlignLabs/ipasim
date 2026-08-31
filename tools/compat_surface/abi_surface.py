#!/usr/bin/env python3
"""Clang-backed AAPCS64 ABI lowering surface for ipaSim.

This tool consumes the typed compatibility inventory and the same SDK/header tree
that produced its C signatures. It asks Clang to lower those declarations for the
inventory's ARM64 iOS target, then converts the resulting LLVM IR declarations
into a deterministic guest-ABI plan.

The plan is mechanical evidence only. It does not generate a callable host bridge,
claim semantic compatibility, or hide unsupported/no-prototype/variadic/callback
cases.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


class AbiSurfaceError(ValueError):
    """Raised when Clang ABI evidence cannot be produced or classified safely."""


@dataclass(frozen=True)
class SelectedSymbol:
    symbol: str
    c_name: str
    source_header: str
    signature: dict


@dataclass(frozen=True)
class LoweredParameter:
    raw: str
    ir_type: str
    attrs: tuple[str, ...]
    hidden_result: bool


@dataclass(frozen=True)
class LoweredDeclaration:
    ir_name: str
    raw: str
    return_type: str
    parameters: tuple[LoweredParameter, ...]
    variadic: bool


def _mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise AbiSurfaceError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise AbiSurfaceError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise AbiSurfaceError(f"{label} must be a non-empty string")
    return value


def load_manifest(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise AbiSurfaceError(
            f"could not read inventory {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise AbiSurfaceError(
            f"inventory {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, "inventory")


def _validate_inventory(manifest: dict) -> tuple[str, list[SelectedSymbol]]:
    if manifest.get("schema_version") != 1:
        raise AbiSurfaceError(
            f"inventory schema_version must be 1, got {manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "typed-compatibility-inventory":
        raise AbiSurfaceError(
            "inventory kind must be 'typed-compatibility-inventory'"
        )
    targets = _mapping(manifest.get("targets"), "inventory targets")
    clang_target = _string(targets.get("clang"), "inventory Clang target")
    lowered_target = clang_target.lower()
    if not (
        lowered_target.startswith("arm64-")
        or lowered_target.startswith("arm64e-")
    ) or "-apple-ios" not in lowered_target:
        raise AbiSurfaceError(
            f"inventory target is not ARM64 iOS: {clang_target!r}"
        )

    selected: list[SelectedSymbol] = []
    seen: set[str] = set()
    symbols = _list(manifest.get("symbols"), "inventory symbols")
    for index, raw_item in enumerate(symbols):
        item = _mapping(raw_item, f"inventory symbols[{index}]")
        symbol = _string(item.get("symbol"), f"inventory symbols[{index}].symbol")
        if symbol in seen:
            raise AbiSurfaceError(f"duplicate inventory symbol {symbol!r}")
        seen.add(symbol)
        signature = item.get("signature")
        if signature is None:
            continue
        signature = _mapping(signature, f"signature for {symbol}")
        if _string(signature.get("symbol"), f"signature symbol for {symbol}") != symbol:
            raise AbiSurfaceError(f"inventory/signature symbol mismatch for {symbol}")
        names = sorted(
            {
                _string(name, f"C name for {symbol}")
                for name in _list(signature.get("names"), f"signature names for {symbol}")
            }
        )
        if not names:
            raise AbiSurfaceError(f"signature for {symbol} has no C identifier")
        sources = _list(signature.get("sources"), f"signature sources for {symbol}")
        headers = []
        for raw_source in sources:
            source = _mapping(raw_source, f"signature source for {symbol}")
            headers.append(
                _string(
                    source.get("header"),
                    f"signature source header for {symbol}",
                )
            )
        if not headers:
            raise AbiSurfaceError(f"signature for {symbol} has no source header")
        selected.append(
            SelectedSymbol(
                symbol=symbol,
                c_name=names[0],
                source_header=sorted(set(headers))[0],
                signature=signature,
            )
        )
    return clang_target, sorted(selected, key=lambda item: item.symbol)


def _resolve_header(root: Path, relative: str) -> Path:
    rel = Path(relative)
    if rel.is_absolute() or ".." in rel.parts:
        raise AbiSurfaceError(
            f"source header must stay inside header root: {relative}"
        )
    path = (root / rel).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise AbiSurfaceError(
            f"source header escapes header root: {relative}"
        ) from exc
    if not path.is_file():
        raise AbiSurfaceError(
            f"source header does not exist under header root: {relative}"
        )
    return path


def _sanitize(text: str, replacements: Sequence[tuple[str, str]]) -> str:
    result = text
    for source, replacement in sorted(
        replacements, key=lambda item: len(item[0]), reverse=True
    ):
        if not source:
            continue
        variants = {
            source,
            source.replace("\\", "/"),
            source.replace("/", "\\"),
        }
        for variant in variants:
            result = result.replace(variant, replacement)
    return result


def _probe_source(
    selected: Sequence[SelectedSymbol], root: Path
) -> tuple[str, dict[str, SelectedSymbol]]:
    includes = sorted({item.source_header for item in selected})
    lines = []
    for relative in includes:
        path = _resolve_header(root, relative)
        escaped = (
            str(path)
            .replace("\\", "/")
            .replace('"', '\\"')
        )
        lines.append(f'#include "{escaped}"')
    mapping: dict[str, SelectedSymbol] = {}
    for index, item in enumerate(selected):
        helper = f"__ipasim_abi_ref_{index:06d}"
        mapping[helper] = item
        lines.append(
            f"__attribute__((used)) __typeof__(&{item.c_name}) "
            f"{helper} = &{item.c_name};"
        )
    return "\n".join(lines) + "\n", mapping


def _run_clang(
    source: str,
    *,
    clang: str,
    target: str,
    header_root: Path,
    sdk_root: Path | None,
    extra_args: Sequence[str],
    timeout_seconds: int,
) -> str:
    with tempfile.TemporaryDirectory(prefix="ipasim-abi-surface-") as directory:
        probe = Path(directory) / "abi_probe.m"
        probe.write_text(source, encoding="utf-8")
        args = [
            clang,
            "-target",
            target,
            "-x",
            "objective-c",
            "-S",
            "-emit-llvm",
            "-O0",
            "-fno-builtin",
            "-fblocks",
            "-Wno-everything",
            "-o",
            "-",
        ]
        if sdk_root is not None:
            root = str(sdk_root)
            args += ["-isysroot", root]
            usr_include = sdk_root / "usr" / "include"
            if usr_include.exists():
                args += ["-isystem", str(usr_include)]
            frameworks = sdk_root / "System" / "Library" / "Frameworks"
            if frameworks.exists():
                args += ["-F", str(frameworks)]
        args += list(extra_args)
        args.append(str(probe))
        replacements = [
            (str(probe), "<ABI_PROBE>"),
            (str(header_root), "<HEADER_ROOT>"),
        ]
        if sdk_root is not None:
            replacements.append((str(sdk_root), "<SDKROOT>"))
        try:
            completed = subprocess.run(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_seconds,
                check=False,
            )
        except FileNotFoundError as exc:
            raise AbiSurfaceError(
                f"Clang executable not found: {clang}"
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise AbiSurfaceError(
                f"Clang ABI probe timed out after {timeout_seconds} seconds"
            ) from exc
        if completed.returncode != 0:
            diagnostic = _sanitize(completed.stderr.strip(), replacements)
            if len(diagnostic) > 12000:
                diagnostic = diagnostic[-12000:]
            raise AbiSurfaceError(
                f"Clang ABI probe failed with exit code {completed.returncode}:\n"
                f"{diagnostic}"
            )
        return completed.stdout


_IR_NAME = r'(?:"(?:\\.|[^"\\])*"|[-A-Za-z$._0-9]+)'


def _parse_named_types(ir: str) -> dict[str, str]:
    result: dict[str, str] = {}
    pattern = re.compile(rf'^%({_IR_NAME})\s*=\s*type\s+(.+)$')
    for line in ir.splitlines():
        match = pattern.match(line.strip())
        if match:
            result["%" + match.group(1)] = match.group(2).strip()
    return result


def _parse_ref_targets(ir: str) -> dict[str, str]:
    result: dict[str, str] = {}
    pattern = re.compile(
        rf'^@(__ipasim_abi_ref_\d+)\s*=.*\bptr\s+@({_IR_NAME})(?:\s|,|$)'
    )
    for line in ir.splitlines():
        match = pattern.match(line.strip())
        if match:
            result[match.group(1)] = match.group(2)
    return result


def _find_balanced(
    text: str, start: int, opener: str, closer: str
) -> int:
    depth = 0
    in_quote = False
    escaped = False
    for index in range(start, len(text)):
        char = text[index]
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
        if char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return index
    raise AbiSurfaceError(
        f"unbalanced LLVM IR type/expression: {text!r}"
    )


def _split_top_level(text: str, delimiter: str = ",") -> list[str]:
    parts = []
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
        elif char in ")]}>" and stack:
            if stack[-1] == pairs[char]:
                stack.pop()
        elif char == delimiter and not stack:
            parts.append(text[start:index].strip())
            start = index + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def _leading_ir_type(fragment: str) -> str:
    text = fragment.strip()
    if not text:
        raise AbiSurfaceError("empty LLVM IR parameter")
    if text == "...":
        return "..."
    if text[0] in "[{<":
        close = {"[": "]", "{": "}", "<": ">"}[text[0]]
        end = _find_balanced(text, 0, text[0], close)
        return text[: end + 1]
    if text.startswith('%"'):
        escaped = False
        for index in range(2, len(text)):
            char = text[index]
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                return text[: index + 1]
        raise AbiSurfaceError(
            f"unterminated quoted LLVM IR type: {fragment!r}"
        )
    return text.split(None, 1)[0]


def _trailing_ir_type(prefix: str) -> str:
    text = prefix.strip()
    if text.startswith("declare"):
        text = text[len("declare") :].strip()
    if not text:
        raise AbiSurfaceError("LLVM declaration has no return type")
    if text[-1] in "]}>":
        opener = {"]": "[", "}": "{", ">": "<"}[text[-1]]
        closer = text[-1]
        depth = 0
        in_quote = False
        escaped = False
        for index in range(len(text) - 1, -1, -1):
            char = text[index]
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
            if char == closer:
                depth += 1
            elif char == opener:
                depth -= 1
                if depth == 0:
                    return text[index:]
        raise AbiSurfaceError(
            f"unbalanced LLVM IR return type: {prefix!r}"
        )
    if text.endswith('"'):
        marker = text.rfind('%"')
        if marker >= 0:
            return text[marker:]
    return text.rsplit(None, 1)[-1]


def _parameter_attrs(fragment: str, ir_type: str) -> tuple[str, ...]:
    rest = fragment[len(ir_type) :].strip()
    attrs = []
    pattern = (
        r'\b(?:sret|byval|byref|inalloca|preallocated)\([^)]*\)'
        r'|\b(?:noundef|nonnull|nocapture|readonly|readnone|writeonly|inreg|'
        r'signext|zeroext|swiftself|swifterror|nest|returned|immarg)\b'
    )
    for token in re.findall(pattern, rest):
        attrs.append(token)
    return tuple(attrs)


def _parse_declarations(
    ir: str, ref_targets: dict[str, str]
) -> dict[str, LoweredDeclaration]:
    declarations = [
        line.strip()
        for line in ir.splitlines()
        if line.strip().startswith("declare ")
    ]
    result = {}
    for helper, ir_name in ref_targets.items():
        needle = "@" + ir_name + "("
        candidates = [line for line in declarations if needle in line]
        if len(candidates) != 1:
            raise AbiSurfaceError(
                f"expected one LLVM declaration for @{ir_name}, "
                f"got {len(candidates)}"
            )
        raw = candidates[0]
        marker = raw.index(needle)
        prefix = raw[:marker]
        open_paren = marker + len(needle) - 1
        close_paren = _find_balanced(raw, open_paren, "(", ")")
        params_text = raw[open_paren + 1 : close_paren]
        parameters = []
        variadic = False
        for fragment in _split_top_level(params_text):
            if fragment == "...":
                variadic = True
                continue
            ir_type = _leading_ir_type(fragment)
            attrs = _parameter_attrs(fragment, ir_type)
            parameters.append(
                LoweredParameter(
                    raw=fragment,
                    ir_type=ir_type,
                    attrs=attrs,
                    hidden_result=any(
                        attr.startswith("sret(") for attr in attrs
                    ),
                )
            )
        result[helper] = LoweredDeclaration(
            ir_name=ir_name,
            raw=raw,
            return_type=_trailing_ir_type(prefix),
            parameters=tuple(parameters),
            variadic=variadic,
        )
    return result


def _split_array(ir_type: str) -> tuple[int, str] | None:
    match = re.fullmatch(r'\[\s*(\d+)\s+x\s+(.+)\]', ir_type.strip())
    if not match:
        return None
    return int(match.group(1)), match.group(2).strip()


def _split_vector(ir_type: str) -> tuple[int, str] | None:
    match = re.fullmatch(r'<\s*(\d+)\s+x\s+(.+)>', ir_type.strip())
    if not match:
        return None
    return int(match.group(1)), match.group(2).strip()


def _type_class(
    ir_type: str,
    named_types: dict[str, str],
    seen: set[str] | None = None,
) -> dict:
    value = ir_type.strip()
    seen = set() if seen is None else set(seen)
    if value == "void":
        return {"bank": "none", "slots": 0, "kind": "void"}
    if value == "ptr":
        return {"bank": "gpr", "slots": 1, "kind": "pointer"}
    match = re.fullmatch(r'i(\d+)', value)
    if match:
        bits = int(match.group(1))
        if bits <= 0 or bits > 128:
            return {
                "bank": "unknown",
                "slots": None,
                "kind": "integer",
                "bits": bits,
            }
        return {
            "bank": "gpr",
            "slots": max(1, math.ceil(bits / 64)),
            "kind": "integer",
            "bits": bits,
        }
    if value in {"half", "bfloat", "float", "double", "fp128"}:
        return {"bank": "simd", "slots": 1, "kind": "floating"}
    array = _split_array(value)
    if array:
        count, element = array
        element_class = _type_class(element, named_types, seen)
        if (
            element_class["bank"] in {"gpr", "simd"}
            and isinstance(element_class.get("slots"), int)
        ):
            return {
                "bank": element_class["bank"],
                "slots": count * element_class["slots"],
                "kind": "array",
                "count": count,
                "element": element_class,
            }
        return {
            "bank": "unknown",
            "slots": None,
            "kind": "array",
            "count": count,
            "element": element_class,
        }
    vector = _split_vector(value)
    if vector:
        count, element = vector
        return {
            "bank": "simd",
            "slots": 1,
            "kind": "vector",
            "count": count,
            "element": _type_class(element, named_types, seen),
        }
    if value.startswith("{") and value.endswith("}"):
        fields = _split_top_level(value[1:-1])
        classes = [_type_class(field, named_types, seen) for field in fields]
        banks = {
            item["bank"]
            for item in classes
            if item["bank"] != "none"
        }
        if (
            len(banks) == 1
            and banks <= {"gpr", "simd"}
            and all(isinstance(item.get("slots"), int) for item in classes)
        ):
            bank = next(iter(banks)) if banks else "none"
            return {
                "bank": bank,
                "slots": sum(item["slots"] for item in classes),
                "kind": "struct",
                "fields": classes,
            }
        return {
            "bank": "unknown",
            "slots": None,
            "kind": "struct",
            "fields": classes,
        }
    if value.startswith("%"):
        if value in seen:
            return {
                "bank": "unknown",
                "slots": None,
                "kind": "recursive-named",
                "name": value,
            }
        body = named_types.get(value)
        if body is None:
            return {
                "bank": "unknown",
                "slots": None,
                "kind": "named",
                "name": value,
            }
        seen.add(value)
        classified = _type_class(body, named_types, seen)
        return {
            "bank": classified["bank"],
            "slots": classified.get("slots"),
            "kind": "named",
            "name": value,
            "body": classified,
        }
    return {
        "bank": "unknown",
        "slots": None,
        "kind": "other",
        "ir_type": value,
    }


def _contains_callback(desc: object) -> bool:
    if not isinstance(desc, dict):
        return False
    kind = desc.get("kind")
    if kind == "block-pointer":
        return True
    if kind in {"pointer", "objc-pointer"}:
        pointee = desc.get("pointee")
        if isinstance(pointee, dict) and pointee.get("kind") == "function":
            return True
    for value in desc.values():
        if isinstance(value, dict) and _contains_callback(value):
            return True
        if isinstance(value, list) and any(
            _contains_callback(item) for item in value
        ):
            return True
    return False


def _source_type(signature: dict, index: int) -> dict:
    params = _list(
        signature.get("parameters"),
        f"signature parameters for {signature.get('symbol')}",
    )
    if index >= len(params):
        raise AbiSurfaceError(
            "Clang lowering has more fixed parameters than source signature "
            f"for {signature.get('symbol')}"
        )
    return _mapping(
        params[index].get("type"),
        f"source parameter {index} type",
    )


def _allocate_parameter_locations(parameters: list[dict]) -> None:
    next_gpr = 0
    next_simd = 0
    for parameter in parameters:
        cls = parameter["abi_class"]
        bank = cls.get("bank")
        slots = cls.get("slots")
        if (
            bank not in {"gpr", "simd"}
            or not isinstance(slots, int)
            or slots <= 0
        ):
            parameter["location"] = {"kind": "unknown"}
            continue
        if bank == "gpr":
            if next_gpr + slots <= 8:
                regs = [
                    f"x{index}"
                    for index in range(next_gpr, next_gpr + slots)
                ]
                next_gpr += slots
                parameter["location"] = {
                    "kind": "registers",
                    "bank": "gpr",
                    "registers": regs,
                }
            else:
                next_gpr = 8
                parameter["location"] = {
                    "kind": "stack",
                    "bank": "gpr",
                    "stack_offset": None,
                }
        else:
            if next_simd + slots <= 8:
                regs = [
                    f"v{index}"
                    for index in range(next_simd, next_simd + slots)
                ]
                next_simd += slots
                parameter["location"] = {
                    "kind": "registers",
                    "bank": "simd",
                    "registers": regs,
                }
            else:
                next_simd = 8
                parameter["location"] = {
                    "kind": "stack",
                    "bank": "simd",
                    "stack_offset": None,
                }


def _result_location(
    return_type: str,
    hidden_sret: LoweredParameter | None,
    named_types: dict[str, str],
) -> dict:
    if hidden_sret is not None:
        return {
            "kind": "indirect-result",
            "register": "x8",
            "llvm_parameter": hidden_sret.raw,
        }
    cls = _type_class(return_type, named_types)
    bank = cls.get("bank")
    slots = cls.get("slots")
    if bank == "none":
        return {"kind": "void", "abi_class": cls}
    if (
        bank in {"gpr", "simd"}
        and isinstance(slots, int)
        and slots > 0
    ):
        prefix = "x" if bank == "gpr" else "v"
        return {
            "kind": "registers",
            "bank": bank,
            "registers": [f"{prefix}{index}" for index in range(slots)],
            "abi_class": cls,
        }
    return {"kind": "unknown", "abi_class": cls}


def _bridge_status(
    signature: dict,
    parameters: Sequence[dict],
    result: dict,
    declaration: LoweredDeclaration,
) -> tuple[str, list[str]]:
    if not bool(signature.get("prototype")):
        return (
            "unsupported-no-prototype",
            ["source declaration has no prototype"],
        )
    reasons = []
    if declaration.variadic or bool(signature.get("variadic")):
        reasons.append("variadic tail requires runtime handling")
    if result.get("kind") == "unknown" or any(
        item.get("location", {}).get("kind") == "unknown"
        for item in parameters
    ):
        reasons.append("one or more lowered ABI classes are not understood")
    callback = _contains_callback(signature.get("return_type")) or any(
        _contains_callback(item.get("source_type"))
        for item in parameters
    )
    if callback:
        reasons.append(
            "function/block pointer requires callback trampoline policy"
        )
    if any("not understood" in item for item in reasons):
        return "needs-manual-abi", reasons
    if callback:
        return "callback-runtime", reasons
    if declaration.variadic or bool(signature.get("variadic")):
        return "variadic-runtime", reasons
    return "generated-bridge-candidate", []


def _empty_manifest(target: str) -> dict:
    return {
        "schema_version": 1,
        "kind": "aapcs64-abi-surface",
        "target": target,
        "summary": {
            "typed_symbol_count": 0,
            "generated_bridge_candidate_count": 0,
            "callback_runtime_count": 0,
            "variadic_runtime_count": 0,
            "needs_manual_abi_count": 0,
            "unsupported_no_prototype_count": 0,
            "indirect_result_count": 0,
            "indirect_aggregate_argument_count": 0,
        },
        "symbols": [],
    }


def build_abi_manifest(
    inventory: dict,
    *,
    header_root: Path,
    clang: str = "clang",
    sdk_root: Path | None = None,
    extra_args: Sequence[str] = (),
    timeout_seconds: int = 120,
) -> dict:
    target, selected = _validate_inventory(inventory)
    header_root = header_root.resolve()
    if not header_root.is_dir():
        raise AbiSurfaceError(
            f"header root does not exist: {header_root.name}"
        )
    if sdk_root is not None:
        sdk_root = sdk_root.resolve()
        if not sdk_root.is_dir():
            raise AbiSurfaceError(
                f"SDK root does not exist: {sdk_root.name}"
            )
    if not selected:
        return _empty_manifest(target)

    source, helper_map = _probe_source(selected, header_root)
    ir = _run_clang(
        source,
        clang=clang,
        target=target,
        header_root=header_root,
        sdk_root=sdk_root,
        extra_args=extra_args,
        timeout_seconds=timeout_seconds,
    )
    named_types = _parse_named_types(ir)
    ref_targets = _parse_ref_targets(ir)
    if set(ref_targets) != set(helper_map):
        missing = sorted(set(helper_map) - set(ref_targets))
        extra = sorted(set(ref_targets) - set(helper_map))
        raise AbiSurfaceError(
            "LLVM IR reference map mismatch; "
            f"missing={missing}, extra={extra}"
        )
    declarations = _parse_declarations(ir, ref_targets)

    rendered = []
    counts = {
        "generated-bridge-candidate": 0,
        "callback-runtime": 0,
        "variadic-runtime": 0,
        "needs-manual-abi": 0,
        "unsupported-no-prototype": 0,
    }
    indirect_results = 0
    indirect_args = 0
    for helper in sorted(helper_map):
        item = helper_map[helper]
        declaration = declarations[helper]
        hidden = [
            param
            for param in declaration.parameters
            if param.hidden_result
        ]
        if len(hidden) > 1:
            raise AbiSurfaceError(
                f"{item.symbol}: multiple sret parameters in Clang lowering"
            )
        hidden_sret = hidden[0] if hidden else None
        lowered_fixed = [
            param
            for param in declaration.parameters
            if not param.hidden_result
        ]
        source_params = _list(
            item.signature.get("parameters"),
            f"signature parameters for {item.symbol}",
        )
        if len(lowered_fixed) != len(source_params):
            raise AbiSurfaceError(
                f"{item.symbol}: lowered/source fixed parameter count mismatch: "
                f"{len(lowered_fixed)} vs {len(source_params)}"
            )

        params = []
        for index, lowered in enumerate(lowered_fixed):
            source_type = _source_type(item.signature, index)
            abi_class = _type_class(lowered.ir_type, named_types)
            indirect_source_aggregate = (
                source_type.get("kind") in {"record", "array"}
                and lowered.ir_type == "ptr"
            )
            if indirect_source_aggregate:
                indirect_args += 1
            params.append(
                {
                    "index": index,
                    "source_type": source_type,
                    "lowered_ir_type": lowered.ir_type,
                    "llvm_parameter": lowered.raw,
                    "llvm_attributes": list(lowered.attrs),
                    "abi_class": abi_class,
                    "indirect_source_aggregate": indirect_source_aggregate,
                }
            )
        _allocate_parameter_locations(params)
        result = _result_location(
            declaration.return_type,
            hidden_sret,
            named_types,
        )
        if result["kind"] == "indirect-result":
            indirect_results += 1
        status, reasons = _bridge_status(
            item.signature,
            params,
            result,
            declaration,
        )
        counts[status] += 1
        rendered.append(
            {
                "symbol": item.symbol,
                "c_name": item.c_name,
                "source_header": item.source_header,
                "calling_convention": item.signature.get(
                    "calling_convention"
                ),
                "prototype": bool(item.signature.get("prototype")),
                "variadic": bool(item.signature.get("variadic")),
                "llvm_ir_name": declaration.ir_name,
                "llvm_ir_declaration": declaration.raw,
                "return": {
                    "source_type": item.signature.get("return_type"),
                    "lowered_ir_type": declaration.return_type,
                    "location": result,
                },
                "parameters": params,
                "bridge_status": status,
                "bridge_reasons": reasons,
            }
        )
    rendered.sort(key=lambda item: item["symbol"])
    return {
        "schema_version": 1,
        "kind": "aapcs64-abi-surface",
        "target": target,
        "summary": {
            "typed_symbol_count": len(rendered),
            "generated_bridge_candidate_count": counts[
                "generated-bridge-candidate"
            ],
            "callback_runtime_count": counts["callback-runtime"],
            "variadic_runtime_count": counts["variadic-runtime"],
            "needs_manual_abi_count": counts["needs-manual-abi"],
            "unsupported_no_prototype_count": counts[
                "unsupported-no-prototype"
            ],
            "indirect_result_count": indirect_results,
            "indirect_aggregate_argument_count": indirect_args,
        },
        "symbols": rendered,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--inventory",
        required=True,
        help="typed compatibility inventory JSON",
    )
    parser.add_argument(
        "--header-root",
        required=True,
        help="root used to resolve signature source_header paths",
    )
    parser.add_argument(
        "--sdk-root",
        help="optional Apple SDK root passed to Clang as -isysroot",
    )
    parser.add_argument(
        "--clang",
        default=os.environ.get("CLANG", "clang"),
    )
    parser.add_argument("--clang-arg", action="append", default=[])
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--output")
    args = parser.parse_args(argv)
    try:
        if args.timeout <= 0:
            raise AbiSurfaceError("timeout must be positive")
        if (
            shutil.which(args.clang) is None
            and not Path(args.clang).is_file()
        ):
            raise AbiSurfaceError(
                f"Clang executable not found: {args.clang}"
            )
        inventory = load_manifest(Path(args.inventory))
        manifest = build_abi_manifest(
            inventory,
            header_root=Path(args.header_root),
            clang=args.clang,
            sdk_root=Path(args.sdk_root) if args.sdk_root else None,
            extra_args=args.clang_arg,
            timeout_seconds=args.timeout,
        )
        rendered = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (AbiSurfaceError, OSError) as exc:
        print(f"[abi-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
