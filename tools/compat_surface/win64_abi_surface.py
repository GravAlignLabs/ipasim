#!/usr/bin/env python3
"""Compiler-backed Win64 carrier ABI surface for ipaSim.

Consumes the AAPCS64 ABI surface and lowers equivalent fixed-width carrier
signatures for x86_64 Windows. The carrier preserves the guest lowering's value
width/shape rather than recompiling Apple source spellings under Windows LLP64.
It is mechanical bridge evidence, not a semantic host implementation.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

DEFAULT_HOST_TARGET = "x86_64-pc-windows-msvc"
_HOST_GPRS = ("rcx", "rdx", "r8", "r9")
_HOST_SIMDS = ("xmm0", "xmm1", "xmm2", "xmm3")


class Win64AbiError(ValueError):
    """Raised when host carrier ABI evidence cannot be produced safely."""


class CarrierTypeError(ValueError):
    """Raised when a guest-lowered LLVM type has no safe carrier spelling."""


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
        raise Win64AbiError(f"{label} must be a JSON object")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise Win64AbiError(f"{label} must be a JSON array")
    return value


def _string(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise Win64AbiError(f"{label} must be a non-empty string")
    return value


def load_manifest(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise Win64AbiError(
            f"could not read guest ABI manifest {path.name}: {exc.strerror or exc}"
        ) from exc
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        raise Win64AbiError(
            f"guest ABI manifest {path.name} is not valid JSON: {exc.msg}"
        ) from exc
    return _mapping(value, "guest ABI manifest")


def _validate_guest(manifest: dict) -> tuple[str, list[dict]]:
    if manifest.get("schema_version") != 1:
        raise Win64AbiError(
            f"guest ABI schema_version must be 1, got {manifest.get('schema_version')!r}"
        )
    if manifest.get("kind") != "aapcs64-abi-surface":
        raise Win64AbiError("guest ABI kind must be 'aapcs64-abi-surface'")
    target = _string(manifest.get("target"), "guest ABI target")
    lowered = target.lower()
    if not (
        lowered.startswith("arm64-") or lowered.startswith("arm64e-")
    ) or "-apple-ios" not in lowered:
        raise Win64AbiError(f"guest ABI target is not ARM64 iOS: {target!r}")
    symbols = _list(manifest.get("symbols"), "guest ABI symbols")
    seen = set()
    result = []
    for index, raw in enumerate(symbols):
        item = _mapping(raw, f"guest ABI symbols[{index}]")
        symbol = _string(item.get("symbol"), f"guest ABI symbols[{index}].symbol")
        if symbol in seen:
            raise Win64AbiError(f"duplicate guest ABI symbol {symbol!r}")
        seen.add(symbol)
        _string(item.get("bridge_status"), f"guest bridge_status for {symbol}")
        _mapping(item.get("return"), f"guest return for {symbol}")
        _list(item.get("parameters"), f"guest parameters for {symbol}")
        result.append(item)
    return target, sorted(result, key=lambda item: item["symbol"])


def _validate_host_target(target: str) -> str:
    lowered = target.lower()
    if not lowered.startswith("x86_64-") or "windows" not in lowered:
        raise Win64AbiError(
            f"host target must be x86_64 Windows, got {target!r}"
        )
    return target


def _find_balanced(text: str, start: int, opener: str, closer: str) -> int:
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
    raise Win64AbiError(f"unbalanced expression: {text!r}")


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
        elif char in ")]}>" and stack and stack[-1] == pairs[char]:
            stack.pop()
        elif char == delimiter and not stack:
            parts.append(text[start:index].strip())
            start = index + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def _split_array(ir_type: str) -> tuple[int, str] | None:
    match = re.fullmatch(r"\[\s*(\d+)\s+x\s+(.+)\]", ir_type.strip())
    if not match:
        return None
    return int(match.group(1)), match.group(2).strip()


def _split_vector(ir_type: str) -> tuple[int, str] | None:
    value = ir_type.strip()
    if value.startswith("<{"):
        return None
    match = re.fullmatch(r"<\s*(\d+)\s+x\s+(.+)>", value)
    if not match:
        return None
    return int(match.group(1)), match.group(2).strip()


def _scalar_c_type(ir_type: str) -> str | None:
    value = ir_type.strip()
    integer = re.fullmatch(r"i(\d+)", value)
    if integer:
        bits = int(integer.group(1))
        return {
            1: "_Bool",
            8: "signed char",
            16: "short",
            32: "int",
            64: "long long",
            128: "__int128",
        }.get(bits)
    return {
        "ptr": "void *",
        "half": "_Float16",
        "float": "float",
        "double": "double",
    }.get(value)


def _scalar_size(ir_type: str) -> int | None:
    value = ir_type.strip()
    integer = re.fullmatch(r"i(\d+)", value)
    if integer:
        bits = int(integer.group(1))
        if bits % 8 == 0:
            return bits // 8
        return None
    return {
        "half": 2,
        "float": 4,
        "double": 8,
    }.get(value)


class CarrierRenderer:
    def __init__(self) -> None:
        self.declarations: list[str] = []
        self._cache: dict[tuple[str, str], str] = {}
        self._counter = 0

    def render(self, ir_type: str, hint: str) -> str:
        value = ir_type.strip()
        scalar = _scalar_c_type(value)
        if scalar is not None:
            return scalar
        if value == "void":
            return "void"
        if value.startswith("%"):
            raise CarrierTypeError(
                f"unresolved named LLVM type {value}; preserve its layout before Win64 lowering"
            )
        if value.startswith("<{"):
            raise CarrierTypeError(
                f"packed LLVM struct {value} is not yet a carrier type"
            )
        key = (value, hint)
        if key in self._cache:
            return self._cache[key]
        array = _split_array(value)
        if array is not None:
            count, element = array
            element_c = self.render(element, hint + "_e")
            name = self._name(hint)
            self.declarations.append(
                f"typedef struct {{ {element_c} value[{count}]; }} {name};"
            )
            self._cache[key] = name
            return name
        vector = _split_vector(value)
        if vector is not None:
            count, element = vector
            element_c = _scalar_c_type(element)
            element_size = _scalar_size(element)
            if element_c is None or element_size is None:
                raise CarrierTypeError(
                    f"vector element {element!r} cannot be represented safely"
                )
            name = self._name(hint)
            self.declarations.append(
                f"typedef {element_c} {name} "
                f"__attribute__((vector_size({count * element_size})));"
            )
            self._cache[key] = name
            return name
        if value.startswith("{") and value.endswith("}"):
            fields = _split_top_level(value[1:-1])
            name = self._name(hint)
            rendered_fields = [
                f"{self.render(field, hint + f'_f{index}')} f{index};"
                for index, field in enumerate(fields)
            ]
            self.declarations.append(
                "typedef struct { " + " ".join(rendered_fields) + f" }} {name};"
            )
            self._cache[key] = name
            return name
        raise CarrierTypeError(f"unsupported LLVM carrier type {value!r}")

    def _name(self, hint: str) -> str:
        name = (
            f"__ipasim_carrier_{self._counter:06d}_"
            f"{re.sub(r'[^A-Za-z0-9_]', '_', hint)}"
        )
        self._counter += 1
        return name


def _build_probe(
    symbols: Sequence[dict],
) -> tuple[str, dict[str, dict], dict[str, dict]]:
    renderer = CarrierRenderer()
    helper_map: dict[str, dict] = {}
    adapter_meta: dict[str, dict] = {}
    function_lines: list[str] = []
    for index, item in enumerate(symbols):
        helper = f"__ipasim_host_ref_{index:06d}"
        adapter = f"__ipasim_host_adapter_{index:06d}"
        logical_args = []
        guest_return = _mapping(
            item.get("return"), f"guest return for {item['symbol']}"
        )
        guest_result_location = _mapping(
            guest_return.get("location"),
            f"guest return location for {item['symbol']}",
        )
        if guest_result_location.get("kind") == "indirect-result":
            return_c = "void"
            logical_args.append(
                {
                    "kind": "guest-result-pointer",
                    "source_index": None,
                    "carrier_ir_type": "ptr",
                    "c_type": "void *",
                    "guest_location": guest_result_location,
                }
            )
        else:
            lowered_return = _string(
                guest_return.get("lowered_ir_type"),
                f"guest lowered return type for {item['symbol']}",
            )
            return_c = renderer.render(lowered_return, f"s{index}_ret")

        for param in _list(
            item.get("parameters"), f"guest parameters for {item['symbol']}"
        ):
            param = _mapping(param, f"guest parameter for {item['symbol']}")
            param_index = param.get("index")
            if isinstance(param_index, bool) or not isinstance(param_index, int):
                raise Win64AbiError(
                    f"guest parameter index for {item['symbol']} must be integer"
                )
            lowered = _string(
                param.get("lowered_ir_type"),
                f"guest lowered parameter type {param_index} for {item['symbol']}",
            )
            logical_args.append(
                {
                    "kind": "source-parameter",
                    "source_index": param_index,
                    "carrier_ir_type": lowered,
                    "c_type": renderer.render(
                        lowered, f"s{index}_p{param_index}"
                    ),
                    "guest_location": _mapping(
                        param.get("location"),
                        f"guest parameter location {param_index} "
                        f"for {item['symbol']}",
                    ),
                    "indirect_source_aggregate": bool(
                        param.get("indirect_source_aggregate")
                    ),
                }
            )
        args = ", ".join(
            f"{arg['c_type']} a{arg_index}"
            for arg_index, arg in enumerate(logical_args)
        ) or "void"
        function_lines.append(f"extern {return_c} {adapter}({args});")
        function_lines.append(
            f"__attribute__((used)) __typeof__(&{adapter}) "
            f"{helper} = &{adapter};"
        )
        helper_map[helper] = item
        adapter_meta[helper] = {
            "adapter": adapter,
            "logical_args": logical_args,
            "guest_return": guest_return,
            "carrier_return_c_type": return_c,
        }
    source = "\n".join(renderer.declarations + function_lines) + "\n"
    return source, helper_map, adapter_meta


def _run_clang(
    source: str,
    *,
    clang: str,
    target: str,
    extra_args: Sequence[str],
    timeout_seconds: int,
) -> str:
    with tempfile.TemporaryDirectory(prefix="ipasim-win64-abi-") as directory:
        probe = Path(directory) / "win64_probe.c"
        probe.write_text(source, encoding="utf-8")
        args = [
            clang,
            "-target",
            target,
            "-x",
            "c",
            "-S",
            "-emit-llvm",
            "-O0",
            "-fno-builtin",
            "-Wno-everything",
            "-o",
            "-",
        ]
        args += list(extra_args)
        args.append(str(probe))
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
            raise Win64AbiError(
                f"Clang executable not found: {clang}"
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise Win64AbiError(
                f"Clang Win64 ABI probe timed out after {timeout_seconds} seconds"
            ) from exc
        if completed.returncode != 0:
            diagnostic = completed.stderr.replace(
                str(probe), "<WIN64_ABI_PROBE>"
            ).strip()
            if len(diagnostic) > 12000:
                diagnostic = diagnostic[-12000:]
            raise Win64AbiError(
                f"Clang Win64 ABI probe failed with exit code "
                f"{completed.returncode}:\n{diagnostic}"
            )
        return completed.stdout


def _parse_ref_targets(ir: str) -> dict[str, str]:
    result = {}
    pattern = re.compile(
        r"^@(__ipasim_host_ref_\d+)\s*=.*\bptr\s+"
        r"@(__ipasim_host_adapter_\d+)(?:\s|,|$)"
    )
    for line in ir.splitlines():
        match = pattern.match(line.strip())
        if match:
            result[match.group(1)] = match.group(2)
    return result


def _leading_ir_type(fragment: str) -> str:
    text = fragment.strip()
    if not text:
        raise Win64AbiError("empty LLVM IR parameter")
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
        raise Win64AbiError(
            f"unterminated quoted LLVM IR type: {fragment!r}"
        )
    return text.split(None, 1)[0]


def _trailing_ir_type(prefix: str) -> str:
    text = prefix.strip()
    if text.startswith("declare"):
        text = text[len("declare") :].strip()
    if not text:
        raise Win64AbiError("LLVM declaration has no return type")
    if text[-1] in "]}>":
        opener = {"]": "[", "}": "{", ">": "<"}[text[-1]]
        closer = text[-1]
        depth = 0
        for index in range(len(text) - 1, -1, -1):
            char = text[index]
            if char == closer:
                depth += 1
            elif char == opener:
                depth -= 1
                if depth == 0:
                    return text[index:]
    if text.endswith('"'):
        marker = text.rfind('%"')
        if marker >= 0:
            return text[marker:]
    return text.rsplit(None, 1)[-1]


def _parameter_attrs(fragment: str, ir_type: str) -> tuple[str, ...]:
    rest = fragment[len(ir_type) :].strip()
    pattern = (
        r"\b(?:sret|byval|byref|inalloca|preallocated)\([^)]*\)"
        r"|\b(?:noundef|nonnull|nocapture|readonly|readnone|writeonly|inreg|"
        r"signext|zeroext|swiftself|swifterror|nest|returned|immarg|"
        r"dead_on_unwind|writable)\b"
    )
    return tuple(re.findall(pattern, rest))


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
            raise Win64AbiError(
                f"expected one LLVM declaration for @{ir_name}, "
                f"got {len(candidates)}"
            )
        raw = candidates[0]
        marker = raw.index(needle)
        prefix = raw[:marker]
        open_paren = marker + len(needle) - 1
        close_paren = _find_balanced(raw, open_paren, "(", ")")
        parameters = []
        variadic = False
        for fragment in _split_top_level(raw[open_paren + 1 : close_paren]):
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


def _ir_bank(ir_type: str) -> str:
    value = ir_type.strip()
    if value == "ptr" or re.fullmatch(r"i\d+", value):
        return "gpr"
    if (
        value in {"half", "float", "double", "fp128"}
        or _split_vector(value)
    ):
        return "simd"
    if value == "void":
        return "none"
    return "unknown"


def _host_parameter_locations(
    declaration: LoweredDeclaration,
) -> list[dict]:
    rendered = []
    for position, param in enumerate(declaration.parameters):
        bank = _ir_bank(param.ir_type)
        if position < 4 and bank == "gpr":
            location = {
                "kind": "register",
                "slot": position,
                "bank": "gpr",
                "register": _HOST_GPRS[position],
            }
        elif position < 4 and bank == "simd":
            location = {
                "kind": "register",
                "slot": position,
                "bank": "simd",
                "register": _HOST_SIMDS[position],
            }
        elif position >= 4 and bank in {"gpr", "simd"}:
            location = {
                "kind": "stack",
                "slot": position,
                "bank": bank,
                "stack_offset": None,
            }
        else:
            location = {
                "kind": "unknown",
                "slot": position,
                "bank": bank,
            }
        rendered.append(
            {
                "position": position,
                "lowered_ir_type": param.ir_type,
                "llvm_parameter": param.raw,
                "llvm_attributes": list(param.attrs),
                "hidden_result": param.hidden_result,
                "location": location,
            }
        )
    return rendered


def _host_result(
    declaration: LoweredDeclaration, host_params: Sequence[dict]
) -> dict:
    hidden = [item for item in host_params if item["hidden_result"]]
    if len(hidden) > 1:
        raise Win64AbiError(
            "multiple Win64 sret parameters in carrier lowering"
        )
    if hidden:
        return {
            "kind": "indirect-result",
            "location": hidden[0]["location"],
            "llvm_parameter": hidden[0]["llvm_parameter"],
        }
    bank = _ir_bank(declaration.return_type)
    if bank == "none":
        return {"kind": "void"}
    if bank == "gpr":
        return {
            "kind": "register",
            "bank": "gpr",
            "register": "rax",
        }
    if bank == "simd":
        return {
            "kind": "register",
            "bank": "simd",
            "register": "xmm0",
        }
    return {
        "kind": "unknown",
        "lowered_ir_type": declaration.return_type,
    }


def _transfer_complexity(guest: dict, host: dict) -> str:
    guest_kind = guest.get("kind")
    host_kind = host.get("kind")
    if guest_kind == "registers" and host_kind == "register":
        regs = guest.get("registers") or []
        if len(regs) == 1:
            return "scalar-register-transfer"
        return "repack-or-materialize"
    if guest_kind == "indirect-result" and host_kind == "register":
        return "pointer-register-transfer"
    if guest_kind == "stack" or host_kind == "stack":
        return "stack-transfer"
    return "adapter-required"


def build_win64_manifest(
    guest_manifest: dict,
    *,
    clang: str = "clang",
    host_target: str = DEFAULT_HOST_TARGET,
    extra_args: Sequence[str] = (),
    timeout_seconds: int = 120,
) -> dict:
    guest_target, symbols = _validate_guest(guest_manifest)
    host_target = _validate_host_target(host_target)

    inherited = []
    renderable = []
    carrier_failures = []
    for item in symbols:
        if item["bridge_status"] != "generated-bridge-candidate":
            inherited.append(
                {
                    "symbol": item["symbol"],
                    "guest_bridge_status": item["bridge_status"],
                    "cross_abi_status": "inherited-runtime-boundary",
                    "reasons": list(item.get("bridge_reasons") or []),
                }
            )
            continue
        try:
            # Dry-render one symbol so unsupported carrier types do not poison
            # the batch for otherwise usable symbols.
            _build_probe([item])
            renderable.append(item)
        except CarrierTypeError as exc:
            carrier_failures.append(
                {
                    "symbol": item["symbol"],
                    "guest_bridge_status": item["bridge_status"],
                    "cross_abi_status": "needs-carrier-type",
                    "reasons": [str(exc)],
                }
            )

    compiled_records = []
    if renderable:
        source, helper_map, adapter_meta = _build_probe(renderable)
        ir = _run_clang(
            source,
            clang=clang,
            target=host_target,
            extra_args=extra_args,
            timeout_seconds=timeout_seconds,
        )
        ref_targets = _parse_ref_targets(ir)
        if set(ref_targets) != set(helper_map):
            raise Win64AbiError(
                "Win64 LLVM IR reference map mismatch; "
                f"missing={sorted(set(helper_map) - set(ref_targets))}, "
                f"extra={sorted(set(ref_targets) - set(helper_map))}"
            )
        declarations = _parse_declarations(ir, ref_targets)
        for helper in sorted(helper_map):
            item = helper_map[helper]
            meta = adapter_meta[helper]
            declaration = declarations[helper]
            if declaration.variadic:
                raise Win64AbiError(
                    f"{item['symbol']}: synthesized carrier unexpectedly "
                    "became variadic"
                )
            host_params = _host_parameter_locations(declaration)
            hidden = [
                param for param in host_params if param["hidden_result"]
            ]
            logical_host_params = [
                param for param in host_params if not param["hidden_result"]
            ]
            logical_args = meta["logical_args"]
            if len(logical_host_params) != len(logical_args):
                raise Win64AbiError(
                    f"{item['symbol']}: Win64 lowered/logical parameter "
                    f"count mismatch: {len(logical_host_params)} vs "
                    f"{len(logical_args)}"
                )
            transfers = []
            result_pointer_transfer = None
            for logical, host_param in zip(
                logical_args, logical_host_params
            ):
                transfer = {
                    "kind": logical["kind"],
                    "source_index": logical["source_index"],
                    "carrier_ir_type": logical["carrier_ir_type"],
                    "guest_location": logical["guest_location"],
                    "host_lowered_ir_type": host_param[
                        "lowered_ir_type"
                    ],
                    "host_llvm_attributes": host_param[
                        "llvm_attributes"
                    ],
                    "host_location": host_param["location"],
                    "transfer_complexity": _transfer_complexity(
                        logical["guest_location"],
                        host_param["location"],
                    ),
                }
                if logical.get("indirect_source_aggregate"):
                    transfer["indirect_source_aggregate"] = True
                if logical["kind"] == "guest-result-pointer":
                    result_pointer_transfer = transfer
                else:
                    transfers.append(transfer)
            host_result = _host_result(declaration, host_params)
            guest_return = meta["guest_return"]
            guest_result_location = guest_return["location"]
            if result_pointer_transfer is not None:
                return_transfer = {
                    "kind": "guest-indirect-result-pointer",
                    "guest_location": guest_result_location,
                    "host_carrier_result": {"kind": "void"},
                    "result_pointer_transfer": result_pointer_transfer,
                }
            else:
                return_transfer = {
                    "kind": "value-result",
                    "carrier_ir_type": guest_return.get(
                        "lowered_ir_type"
                    ),
                    "guest_location": guest_result_location,
                    "host_carrier_result": host_result,
                    "transfer_complexity": _transfer_complexity(
                        guest_result_location,
                        host_result.get("location", host_result),
                    ),
                }
            unknown = host_result.get("kind") == "unknown" or any(
                transfer["host_location"].get("kind") == "unknown"
                for transfer in transfers
            )
            status = (
                "needs-manual-host-abi"
                if unknown
                else "cross-abi-adapter-candidate"
            )
            reasons = (
                [
                    "one or more Win64 carrier locations are not understood"
                ]
                if unknown
                else []
            )
            compiled_records.append(
                {
                    "symbol": item["symbol"],
                    "guest_bridge_status": item["bridge_status"],
                    "cross_abi_status": status,
                    "reasons": reasons,
                    "host_target": host_target,
                    "host_llvm_ir_name": declaration.ir_name,
                    "host_llvm_ir_declaration": declaration.raw,
                    "host_hidden_result_parameters": hidden,
                    "parameters": sorted(
                        transfers,
                        key=lambda value: value["source_index"],
                    ),
                    "return": return_transfer,
                }
            )

    rendered = sorted(
        inherited + carrier_failures + compiled_records,
        key=lambda item: item["symbol"],
    )
    counts = {
        "cross_abi_adapter_candidate_count": sum(
            item["cross_abi_status"] == "cross-abi-adapter-candidate"
            for item in rendered
        ),
        "inherited_runtime_boundary_count": sum(
            item["cross_abi_status"] == "inherited-runtime-boundary"
            for item in rendered
        ),
        "needs_carrier_type_count": sum(
            item["cross_abi_status"] == "needs-carrier-type"
            for item in rendered
        ),
        "needs_manual_host_abi_count": sum(
            item["cross_abi_status"] == "needs-manual-host-abi"
            for item in rendered
        ),
    }
    return {
        "schema_version": 1,
        "kind": "win64-carrier-abi-surface",
        "guest_target": guest_target,
        "host_target": host_target,
        "summary": {
            "guest_symbol_count": len(symbols),
            **counts,
        },
        "symbols": rendered,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--guest-abi",
        required=True,
        help="AAPCS64 ABI surface JSON",
    )
    parser.add_argument(
        "--host-target",
        default=DEFAULT_HOST_TARGET,
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
            raise Win64AbiError("timeout must be positive")
        if (
            shutil.which(args.clang) is None
            and not Path(args.clang).is_file()
        ):
            raise Win64AbiError(
                f"Clang executable not found: {args.clang}"
            )
        manifest = build_win64_manifest(
            load_manifest(Path(args.guest_abi)),
            clang=args.clang,
            host_target=args.host_target,
            extra_args=args.clang_arg,
            timeout_seconds=args.timeout,
        )
        rendered = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (Win64AbiError, OSError) as exc:
        print(f"[win64-abi-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
