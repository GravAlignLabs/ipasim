#!/usr/bin/env python3
"""Clang-backed Apple SDK header signature surface for ipaSim.

This tool extracts mechanical C function type information for an ARM64 iOS
target. It does not infer behavior and does not generate callable bridges.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

DEFAULT_TARGET = "arm64-apple-ios16.0"
TYPE_KINDS = {
    "BuiltinType", "PointerType", "BlockPointerType", "LValueReferenceType",
    "RValueReferenceType", "MemberPointerType", "RecordType", "EnumType",
    "FunctionProtoType", "FunctionNoProtoType", "ConstantArrayType",
    "IncompleteArrayType", "VariableArrayType", "DependentSizedArrayType",
    "VectorType", "ExtVectorType", "ComplexType", "AtomicType",
    "ObjCObjectPointerType", "ObjCObjectType", "ObjCInterfaceType",
    "TypedefType", "ElaboratedType", "ParenType", "AttributedType",
    "AdjustedType", "DecayedType", "MacroQualifiedType", "TypeOfExprType",
    "TypeOfType", "DecltypeType", "AutoType", "QualType",
}
TRANSPARENT_TYPES = {
    "TypedefType", "ElaboratedType", "ParenType", "MacroQualifiedType",
    "TypeOfExprType", "TypeOfType", "DecltypeType",
}
SCOPE_BLOCKERS = {
    "NamespaceDecl", "CXXRecordDecl", "RecordDecl", "ClassTemplateDecl",
    "ClassTemplateSpecializationDecl", "FunctionTemplateDecl",
}


class HeaderParseError(ValueError):
    """Raised when Clang cannot produce an unambiguous signature surface."""


@dataclass(frozen=True)
class RawParam:
    name: str | None
    spelling: str


@dataclass(frozen=True)
class RawFunction:
    header: str
    name: str
    symbol: str
    function_type: str
    params: tuple[RawParam, ...]
    variadic: bool
    line: int | None
    column: int | None


@dataclass(frozen=True)
class HeaderSignature:
    header: str
    name: str
    symbol: str
    function_type: str
    calling_convention: str
    variadic: bool
    prototype: bool
    return_type: dict
    parameter_types: tuple[dict, ...]
    params: tuple[RawParam, ...]
    line: int | None
    column: int | None


def _type_children(node: dict) -> list[dict]:
    result = []
    for child in node.get("inner") or []:
        if isinstance(child, dict) and (
            child.get("kind") in TYPE_KINDS
            or str(child.get("kind", "")).endswith("Type")
        ):
            result.append(child)
    return result


def _merge_qualifiers(desc: dict, qualifiers: str | None) -> dict:
    if not qualifiers:
        return desc
    result = dict(desc)
    existing = set(result.get("qualifiers") or [])
    existing.update(item for item in qualifiers.split() if item)
    result["qualifiers"] = sorted(existing)
    return result


def _type_descriptor(node: dict) -> dict:
    kind = str(node.get("kind", "UnknownType"))
    spelling = str((node.get("type") or {}).get("qualType") or "")
    children = _type_children(node)

    if kind == "QualType" and children:
        return _merge_qualifiers(_type_descriptor(children[0]), node.get("qualifiers"))
    if kind in TRANSPARENT_TYPES and children:
        return _type_descriptor(children[-1])
    if kind == "AttributedType":
        base = (
            _type_descriptor(children[-1])
            if children
            else {"kind": "unknown", "spelling": spelling}
        )
        return {"kind": "attributed", "spelling": spelling, "base": base}
    if kind in ("AdjustedType", "DecayedType"):
        base = (
            _type_descriptor(children[-1])
            if children
            else {"kind": "unknown", "spelling": spelling}
        )
        return {"kind": "adjusted", "spelling": spelling, "adjusted": base}
    if kind == "PointerType":
        return {
            "kind": "pointer",
            "pointee": (
                _type_descriptor(children[0])
                if children
                else {"kind": "unknown", "spelling": spelling}
            ),
        }
    if kind == "BlockPointerType":
        return {
            "kind": "block-pointer",
            "pointee": (
                _type_descriptor(children[0])
                if children
                else {"kind": "unknown", "spelling": spelling}
            ),
        }
    if kind in ("LValueReferenceType", "RValueReferenceType"):
        return {
            "kind": "reference",
            "reference_kind": "lvalue" if kind.startswith("L") else "rvalue",
            "referent": (
                _type_descriptor(children[0])
                if children
                else {"kind": "unknown", "spelling": spelling}
            ),
        }
    if kind == "MemberPointerType":
        return {
            "kind": "member-pointer",
            "spelling": spelling,
            "children": [_type_descriptor(item) for item in children],
        }
    if kind in ("FunctionProtoType", "FunctionNoProtoType"):
        if not children:
            return {
                "kind": "function",
                "prototype": kind == "FunctionProtoType",
                "spelling": spelling,
            }
        return {
            "kind": "function",
            "prototype": kind == "FunctionProtoType",
            "calling_convention": node.get("cc", "cdecl"),
            "variadic": bool(node.get("variadic", False)),
            "return": _type_descriptor(children[0]),
            "parameters": [_type_descriptor(item) for item in children[1:]],
        }
    if kind in (
        "ConstantArrayType",
        "IncompleteArrayType",
        "VariableArrayType",
        "DependentSizedArrayType",
    ):
        result = {
            "kind": "array",
            "array_kind": kind.removesuffix("Type"),
            "element": (
                _type_descriptor(children[0])
                if children
                else {"kind": "unknown", "spelling": spelling}
            ),
        }
        if "size" in node:
            result["size"] = node["size"]
        return result
    if kind in ("VectorType", "ExtVectorType"):
        result = {
            "kind": "vector",
            "vector_kind": kind.removesuffix("Type"),
            "spelling": spelling,
        }
        if children:
            result["element"] = _type_descriptor(children[0])
        if "numElements" in node:
            result["count"] = node["numElements"]
        return result
    if kind == "ComplexType":
        return {
            "kind": "complex",
            "element": (
                _type_descriptor(children[0])
                if children
                else {"kind": "unknown", "spelling": spelling}
            ),
        }
    if kind == "AtomicType":
        return {
            "kind": "atomic",
            "value": (
                _type_descriptor(children[0])
                if children
                else {"kind": "unknown", "spelling": spelling}
            ),
        }
    if kind == "ObjCObjectPointerType":
        return {
            "kind": "objc-pointer",
            "pointee": (
                _type_descriptor(children[0])
                if children
                else {"kind": "objc-object", "spelling": spelling}
            ),
        }
    if kind in ("ObjCObjectType", "ObjCInterfaceType"):
        return {"kind": "objc-object", "spelling": spelling}
    if kind == "BuiltinType":
        return {"kind": "builtin", "name": spelling}
    if kind == "RecordType":
        return {"kind": "record", "name": spelling}
    if kind == "EnumType":
        return {"kind": "enum", "name": spelling}

    result = {
        "kind": kind.removesuffix("Type").lower() or "unknown",
        "spelling": spelling,
    }
    if children:
        result["children"] = [_type_descriptor(item) for item in children]
    return result


def _json_objects(text: str) -> list[dict]:
    decoder = json.JSONDecoder()
    objects = []
    index = 0
    length = len(text)
    while index < length:
        while index < length and text[index].isspace():
            index += 1
        if index >= length:
            break
        try:
            value, index = decoder.raw_decode(text, index)
        except json.JSONDecodeError as exc:
            raise HeaderParseError(
                f"Clang AST output is not valid JSON near byte {exc.pos}"
            ) from exc
        if isinstance(value, dict):
            objects.append(value)
    return objects


def _walk(node, ancestors: tuple[str, ...] = ()):
    if isinstance(node, dict):
        yield node, ancestors
        kind = str(node.get("kind", ""))
        next_ancestors = ancestors + ((kind,) if kind else ())
        for child in node.get("inner") or []:
            yield from _walk(child, next_ancestors)
    elif isinstance(node, list):
        for child in node:
            yield from _walk(child, ancestors)


def _source_position(loc: dict) -> tuple[int | None, int | None]:
    for candidate in (
        loc,
        loc.get("expansionLoc") or {},
        loc.get("spellingLoc") or {},
    ):
        line = candidate.get("line")
        col = candidate.get("col")
        if line is not None or col is not None:
            return (
                int(line) if line is not None else None,
                int(col) if col is not None else None,
            )
    return None, None


def _sanitize(text: str, replacements: Sequence[tuple[str, str]]) -> str:
    result = text
    for source, replacement in sorted(
        replacements, key=lambda item: len(item[0]), reverse=True
    ):
        if not source:
            continue
        result = result.replace(source, replacement)
        result = result.replace(source.replace("\\", "/"), replacement)
        result = result.replace(source.replace("/", "\\"), replacement)
    return result


def _clang_base(
    clang: str,
    target: str,
    language: str,
    sdk_root: Path | None,
    extra_args: Sequence[str],
) -> list[str]:
    args = [
        clang,
        "-target",
        target,
        "-x",
        language,
        "-fsyntax-only",
        "-fno-builtin",
        "-fblocks",
        "-Wno-everything",
        "-ferror-limit=50",
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
    return args


def _run_clang(
    args: Sequence[str],
    replacements: Sequence[tuple[str, str]],
    timeout_seconds: int,
) -> str:
    try:
        completed = subprocess.run(
            list(args),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
            check=False,
        )
    except FileNotFoundError as exc:
        raise HeaderParseError(f"Clang executable not found: {args[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        raise HeaderParseError(
            f"Clang timed out after {timeout_seconds} seconds"
        ) from exc
    if completed.returncode != 0:
        diagnostic = _sanitize(completed.stderr.strip(), replacements)
        if len(diagnostic) > 12000:
            diagnostic = diagnostic[-12000:]
        raise HeaderParseError(
            f"Clang failed with exit code {completed.returncode}:\n{diagnostic}"
        )
    return completed.stdout


def _discover_raw_functions(
    ast: dict, header: str
) -> tuple[list[RawFunction], int, int]:
    functions: list[RawFunction] = []
    skipped_cxx = 0
    skipped_static = 0
    for node, ancestors in _walk(ast):
        if node.get("kind") != "FunctionDecl":
            continue
        if node.get("isImplicit"):
            continue
        loc = node.get("loc") or {}
        if loc.get("includedFrom"):
            continue
        if any(kind in SCOPE_BLOCKERS for kind in ancestors):
            skipped_cxx += 1
            continue
        if node.get("storageClass") == "static":
            skipped_static += 1
            continue
        name = node.get("name")
        mangled = node.get("mangledName")
        function_type = (node.get("type") or {}).get("qualType")
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(mangled, str)
            or not mangled
        ):
            continue
        if mangled.startswith("__Z"):
            skipped_cxx += 1
            continue
        if not isinstance(function_type, str) or not function_type:
            raise HeaderParseError(
                f"{header}: function {name} has no Clang type spelling"
            )
        params = []
        for child in node.get("inner") or []:
            if (
                not isinstance(child, dict)
                or child.get("kind") != "ParmVarDecl"
            ):
                continue
            spelling = (child.get("type") or {}).get("qualType")
            if not isinstance(spelling, str) or not spelling:
                raise HeaderParseError(
                    f"{header}: parameter of {name} has no type spelling"
                )
            params.append(RawParam(child.get("name"), spelling))
        line, column = _source_position(loc)
        functions.append(
            RawFunction(
                header=header,
                name=name,
                symbol=mangled,
                function_type=function_type,
                params=tuple(params),
                variadic=bool(node.get("variadic", False)),
                line=line,
                column=column,
            )
        )
    return functions, skipped_cxx, skipped_static


def _find_function_type(node: dict) -> dict | None:
    if node.get("kind") in ("FunctionProtoType", "FunctionNoProtoType"):
        return node
    for child in node.get("inner") or []:
        if isinstance(child, dict):
            result = _find_function_type(child)
            if result is not None:
                return result
    return None


def _helper_source(
    header_path: Path, functions: Sequence[RawFunction]
) -> tuple[str, dict[str, RawFunction]]:
    header = (
        str(header_path.resolve())
        .replace("\\", "/")
        .replace('"', '\\"')
    )
    lines = [f'#include "{header}"']
    mapping = {}
    by_name: dict[str, list[RawFunction]] = {}
    for function in functions:
        by_name.setdefault(function.name, []).append(function)
    for name, group in sorted(by_name.items()):
        symbols = {item.symbol for item in group}
        if len(symbols) != 1:
            raise HeaderParseError(
                f"{functions[0].header if functions else header_path.name}: "
                f"overloaded or conflicting C identifier {name!r} maps to "
                f"{sorted(symbols)}"
            )
        helper = f"__ipasim_signature_{len(mapping):06d}"
        mapping[helper] = group[0]
        lines.append(f"typedef __typeof__({name}) {helper};")
    return "\n".join(lines) + "\n", mapping


def analyze_header(
    path: Path,
    display_name: str | None = None,
    *,
    clang: str = "clang",
    target: str = DEFAULT_TARGET,
    sdk_root: Path | None = None,
    extra_args: Sequence[str] = (),
    timeout_seconds: int = 120,
) -> tuple[list[HeaderSignature], dict]:
    path = path.resolve()
    display = (display_name or path.name).replace("\\", "/")
    if not path.is_file():
        raise HeaderParseError(f"header does not exist: {display}")
    root = sdk_root.resolve() if sdk_root is not None else None
    replacements = [(str(path), display)]
    if root is not None:
        replacements.append((str(root), "<SDKROOT>"))

    first_args = _clang_base(
        clang, target, "objective-c-header", root, extra_args
    )
    first_args += ["-Xclang", "-ast-dump=json", str(path)]
    first_output = _run_clang(first_args, replacements, timeout_seconds)
    objects = _json_objects(first_output)
    if len(objects) != 1:
        raise HeaderParseError(
            f"{display}: expected one Clang translation-unit AST, got {len(objects)}"
        )
    raw, skipped_cxx, skipped_static = _discover_raw_functions(
        objects[0], display
    )
    if not raw:
        return [], {
            "skipped_cxx": skipped_cxx,
            "skipped_static": skipped_static,
            "declarations": 0,
        }

    helper_text, helper_map = _helper_source(path, raw)
    with tempfile.TemporaryDirectory(
        prefix="ipasim-header-surface-"
    ) as directory:
        helper_path = Path(directory) / "signature_probe.m"
        helper_path.write_text(helper_text, encoding="utf-8")
        helper_replacements = replacements + [(directory, "<TMP>")]
        second_args = _clang_base(
            clang, target, "objective-c", root, extra_args
        )
        second_args += [
            "-Xclang",
            "-ast-dump-all=json",
            "-Xclang",
            "-ast-dump-filter",
            "-Xclang",
            "__ipasim_signature_",
            str(helper_path),
        ]
        second_output = _run_clang(
            second_args, helper_replacements, timeout_seconds
        )

    typedefs = {}
    for obj in _json_objects(second_output):
        if (
            obj.get("kind") == "TypedefDecl"
            and obj.get("name") in helper_map
        ):
            typedefs[obj["name"]] = obj
    missing = sorted(set(helper_map) - set(typedefs))
    if missing:
        raise HeaderParseError(
            f"{display}: Clang did not emit helper type metadata for "
            f"{missing[:5]}"
        )

    recovered_by_name: dict[
        str, tuple[dict, str, bool, bool, tuple[dict, ...]]
    ] = {}
    for helper, function in helper_map.items():
        fn_type = _find_function_type(typedefs[helper])
        if fn_type is None:
            raise HeaderParseError(
                f"{display}: helper for {function.name} has no function type"
            )
        children = _type_children(fn_type)
        if not children:
            raise HeaderParseError(
                f"{display}: helper for {function.name} has no return type"
            )
        prototype = fn_type.get("kind") == "FunctionProtoType"
        variadic = bool(fn_type.get("variadic", False))
        param_types = tuple(
            _type_descriptor(item) for item in children[1:]
        )
        if prototype and len(param_types) != len(function.params):
            raise HeaderParseError(
                f"{display}: Clang type tree for {function.name} has "
                f"{len(param_types)} parameters but declaration has "
                f"{len(function.params)}"
            )
        recovered_by_name[function.name] = (
            _type_descriptor(children[0]),
            str(fn_type.get("cc", "cdecl")),
            variadic,
            prototype,
            param_types,
        )

    signatures = []
    for function in raw:
        (
            return_type,
            cc,
            variadic,
            prototype,
            param_types,
        ) = recovered_by_name[function.name]
        if variadic != function.variadic:
            raise HeaderParseError(
                f"{display}: inconsistent variadic metadata for {function.name}"
            )
        signatures.append(
            HeaderSignature(
                header=display,
                name=function.name,
                symbol=function.symbol,
                function_type=function.function_type,
                calling_convention=cc,
                variadic=variadic,
                prototype=prototype,
                return_type=return_type,
                parameter_types=param_types,
                params=function.params,
                line=function.line,
                column=function.column,
            )
        )
    return signatures, {
        "skipped_cxx": skipped_cxx,
        "skipped_static": skipped_static,
        "declarations": len(raw),
    }


def _signature_key(item: HeaderSignature) -> str:
    value = {
        "cc": item.calling_convention,
        "variadic": item.variadic,
        "prototype": item.prototype,
        "return": item.return_type,
        "parameters": list(item.parameter_types),
    }
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def build_manifest(
    signatures: Iterable[HeaderSignature],
    *,
    target: str,
    headers: Sequence[str],
    stats: Sequence[dict] = (),
) -> dict:
    groups: dict[str, list[HeaderSignature]] = {}
    for item in signatures:
        groups.setdefault(item.symbol, []).append(item)

    rendered = []
    declaration_count = 0
    variadic_count = 0
    no_prototype_count = 0
    for symbol in sorted(groups):
        group = groups[symbol]
        declaration_count += len(group)
        keys = {_signature_key(item) for item in group}
        if len(keys) != 1:
            details = sorted({item.function_type for item in group})
            raise HeaderParseError(
                f"conflicting header signatures for {symbol}: {details}"
            )
        first = group[0]
        if first.variadic:
            variadic_count += 1
        if not first.prototype:
            no_prototype_count += 1
        names = sorted({item.name for item in group})
        spellings = sorted({item.function_type for item in group})
        sources = sorted(
            {(item.header, item.line, item.column) for item in group},
            key=lambda value: (
                value[0],
                value[1] or 0,
                value[2] or 0,
            ),
        )
        params = []
        for index in range(len(first.parameter_types)):
            param_names = sorted(
                {
                    item.params[index].name
                    for item in group
                    if item.params[index].name
                }
            )
            param_spellings = sorted(
                {item.params[index].spelling for item in group}
            )
            params.append(
                {
                    "index": index,
                    "names": param_names,
                    "spellings": param_spellings,
                    "type": first.parameter_types[index],
                }
            )
        rendered.append(
            {
                "symbol": symbol,
                "names": names,
                "function_type_spellings": spellings,
                "calling_convention": first.calling_convention,
                "variadic": first.variadic,
                "prototype": first.prototype,
                "return_type": first.return_type,
                "parameters": params,
                "sources": [
                    {
                        "header": header,
                        "line": line,
                        "column": column,
                    }
                    for header, line, column in sources
                ],
            }
        )

    skipped_cxx = sum(
        int(item.get("skipped_cxx", 0)) for item in stats
    )
    skipped_static = sum(
        int(item.get("skipped_static", 0)) for item in stats
    )
    return {
        "schema_version": 1,
        "kind": "header-signature-surface",
        "target": target,
        "summary": {
            "header_count": len(set(headers)),
            "declaration_count": declaration_count,
            "unique_symbol_count": len(rendered),
            "variadic_symbol_count": variadic_count,
            "no_prototype_symbol_count": no_prototype_count,
            "skipped_cxx_declaration_count": skipped_cxx,
            "skipped_static_declaration_count": skipped_static,
        },
        "signatures": rendered,
    }


def _resolve_inputs(
    args: argparse.Namespace,
) -> tuple[list[tuple[Path, str]], Path | None]:
    resolved: list[tuple[Path, str]] = []
    sdk_root = Path(args.sdk_root).resolve() if args.sdk_root else None
    if sdk_root is not None:
        if not sdk_root.is_dir():
            raise HeaderParseError(
                f"SDK root does not exist: {args.sdk_root}"
            )
        if args.relative_header:
            relatives = [Path(item) for item in args.relative_header]
        else:
            relatives = sorted(
                path.relative_to(sdk_root)
                for path in sdk_root.rglob("*.h")
                if path.is_file()
            )
        for relative in relatives:
            if relative.is_absolute() or ".." in relative.parts:
                raise HeaderParseError(
                    f"relative header must stay inside SDK root: {relative}"
                )
            path = (sdk_root / relative).resolve()
            try:
                path.relative_to(sdk_root)
            except ValueError as exc:
                raise HeaderParseError(
                    f"relative header escapes SDK root: {relative}"
                ) from exc
            resolved.append((path, relative.as_posix()))
    for raw in args.headers or []:
        path = Path(raw).resolve()
        resolved.append((path, path.name))
    unique: dict[str, tuple[Path, str]] = {}
    for path, display in resolved:
        unique.setdefault(str(path), (path, display))
    if not unique:
        raise HeaderParseError("no headers were supplied")
    return sorted(unique.values(), key=lambda item: item[1]), sdk_root


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("headers", nargs="*", help="explicit header files")
    parser.add_argument(
        "--sdk-root",
        help="SDK root used for includes and relative-path scanning",
    )
    parser.add_argument(
        "--relative-header",
        action="append",
        help="SDK-root-relative header; repeat as needed",
    )
    parser.add_argument(
        "--target",
        default=DEFAULT_TARGET,
        help=f"Clang target triple (default: {DEFAULT_TARGET})",
    )
    parser.add_argument(
        "--clang",
        default=os.environ.get("CLANG", "clang"),
        help="Clang executable",
    )
    parser.add_argument(
        "--clang-arg",
        action="append",
        default=[],
        help="extra Clang argument; repeat as needed",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="per-Clang-invocation timeout in seconds",
    )
    parser.add_argument(
        "--output",
        help="write JSON manifest to this file instead of stdout",
    )
    args = parser.parse_args(argv)
    try:
        if args.timeout <= 0:
            raise HeaderParseError("timeout must be positive")
        if (
            shutil.which(args.clang) is None
            and not Path(args.clang).is_file()
        ):
            raise HeaderParseError(
                f"Clang executable not found: {args.clang}"
            )
        inputs, sdk_root = _resolve_inputs(args)
        all_signatures = []
        stats = []
        for path, display in inputs:
            signatures, header_stats = analyze_header(
                path,
                display,
                clang=args.clang,
                target=args.target,
                sdk_root=sdk_root,
                extra_args=args.clang_arg,
                timeout_seconds=args.timeout,
            )
            all_signatures.extend(signatures)
            stats.append(header_stats)
        manifest = build_manifest(
            all_signatures,
            target=args.target,
            headers=[display for _, display in inputs],
            stats=stats,
        )
        rendered = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (HeaderParseError, OSError) as exc:
        print(f"[header-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
