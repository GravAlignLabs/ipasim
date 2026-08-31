#!/usr/bin/env python3
"""Offline ARM64 Mach-O compatibility-surface analyzer for ipaSim.

The analyzer is deliberately diagnostic-only. It reads Mach-O dependency
ordinals and LC_DYLD_CHAINED_FIXUPS import metadata and emits a deterministic
JSON manifest. It never rewrites binaries or marks unsupported behavior as
implemented.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable, Sequence

CPU_TYPE_ARM64 = 0x0100000C
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF

LC_LOAD_DYLIB = 0x0000000C
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001F
LC_LAZY_LOAD_DYLIB = 0x00000020
LC_LOAD_UPWARD_DYLIB = 0x80000023
LC_DYLD_CHAINED_FIXUPS = 0x80000034

DYLD_CHAINED_IMPORT = 1
DYLD_CHAINED_IMPORT_ADDEND = 2
DYLD_CHAINED_IMPORT_ADDEND64 = 3

_DYLIB_COMMAND_KINDS = {
    LC_LOAD_DYLIB: "load",
    LC_LOAD_WEAK_DYLIB: "weak-load",
    LC_REEXPORT_DYLIB: "reexport",
    LC_LAZY_LOAD_DYLIB: "lazy-load",
    LC_LOAD_UPWARD_DYLIB: "upward-load",
}

_SPECIAL_ORDINALS = {
    0: "self",
    -1: "main-executable",
    -2: "flat-lookup",
    -3: "weak-lookup",
}

_DEFAULT_SIMULATOR_IMAGES = (
    "usr/lib/system/libsystem_sim_kernel.dylib",
    "usr/lib/system/libsystem_sim_platform.dylib",
    "usr/lib/system/libsystem_sim_pthread.dylib",
)


class ParseError(ValueError):
    """Raised when a Mach-O image cannot be interpreted safely."""


@dataclass(frozen=True)
class Dependency:
    ordinal: int
    install_name: str
    kind: str


@dataclass(frozen=True)
class ImportBinding:
    symbol: str
    ordinal: int
    provider: str | None
    provider_kind: str
    weak: bool
    addend: int


@dataclass(frozen=True)
class ImageSurface:
    image: str
    dependencies: tuple[Dependency, ...]
    imports: tuple[ImportBinding, ...]
    chained_fixups: bool
    chained_import_format: int | None


def _require_range(total: int, offset: int, size: int, what: str) -> None:
    if offset < 0 or size < 0 or offset > total or size > total - offset:
        raise ParseError(f"{what} is outside the image")


def _u32le(data: bytes, offset: int) -> int:
    _require_range(len(data), offset, 4, "32-bit field")
    return struct.unpack_from("<I", data, offset)[0]


def _u64le(data: bytes, offset: int) -> int:
    _require_range(len(data), offset, 8, "64-bit field")
    return struct.unpack_from("<Q", data, offset)[0]


def _cstring(data: bytes, begin: int, end: int, what: str) -> str:
    if begin < 0 or end > len(data) or begin >= end:
        raise ParseError(f"{what} has an invalid range")
    nul = data.find(b"\0", begin, end)
    if nul < 0:
        raise ParseError(f"{what} is not NUL terminated")
    try:
        return data[begin:nul].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ParseError(f"{what} is not valid UTF-8") from exc


def _sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    mask = (1 << bits) - 1
    value &= mask
    return value - (1 << bits) if value & sign else value


def _select_arm64_slice(data: bytes) -> tuple[int, int]:
    _require_range(len(data), 0, 8, "file header")

    # Thin little-endian ARM64 Mach-O.
    if _u32le(data, 0) == MH_MAGIC_64:
        _require_range(len(data), 0, 32, "Mach-O header")
        if _u32le(data, 4) != CPU_TYPE_ARM64:
            raise ParseError("thin Mach-O is not ARM64")
        return 0, len(data)

    # Fat headers are stored in network/big-endian byte order.
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic not in (FAT_MAGIC, FAT_MAGIC_64):
        raise ParseError("unsupported Mach-O/fat magic")

    count = struct.unpack_from(">I", data, 4)[0]
    entry_size = 32 if magic == FAT_MAGIC_64 else 20
    _require_range(len(data), 8, count * entry_size, "fat architecture table")

    for index in range(count):
        entry = 8 + index * entry_size
        cputype = struct.unpack_from(">I", data, entry)[0]
        if cputype != CPU_TYPE_ARM64:
            continue

        if magic == FAT_MAGIC_64:
            offset = struct.unpack_from(">Q", data, entry + 8)[0]
            size = struct.unpack_from(">Q", data, entry + 16)[0]
        else:
            offset = struct.unpack_from(">I", data, entry + 8)[0]
            size = struct.unpack_from(">I", data, entry + 12)[0]

        _require_range(len(data), offset, size, "ARM64 fat slice")
        _require_range(len(data), offset, 32, "ARM64 Mach-O header")
        if _u32le(data, offset) != MH_MAGIC_64 or _u32le(data, offset + 4) != CPU_TYPE_ARM64:
            raise ParseError("ARM64 fat slice is not a little-endian Mach-O 64 image")
        return offset, size

    raise ParseError("fat Mach-O contains no ARM64 slice")


def _parse_dependencies_and_fixups(
    data: bytes, slice_offset: int, slice_size: int
) -> tuple[list[Dependency], tuple[int, int] | None]:
    _require_range(len(data), slice_offset, 32, "Mach-O header")
    if slice_size < 32:
        raise ParseError("Mach-O slice is smaller than its header")

    command_count = _u32le(data, slice_offset + 16)
    command_bytes = _u32le(data, slice_offset + 20)
    commands_begin = slice_offset + 32
    _require_range(len(data), commands_begin, command_bytes, "load-command region")
    if command_bytes > slice_size - 32:
        raise ParseError("load-command region exceeds the ARM64 slice")

    dependencies: list[Dependency] = []
    fixups: tuple[int, int] | None = None
    cursor = commands_begin
    commands_end = commands_begin + command_bytes

    for _ in range(command_count):
        if cursor + 8 > commands_end:
            raise ParseError("Mach-O load-command list ends early")
        command, command_size = struct.unpack_from("<II", data, cursor)
        if command_size < 8 or cursor + command_size > commands_end:
            raise ParseError("invalid Mach-O load-command size")

        kind = _DYLIB_COMMAND_KINDS.get(command)
        if kind is not None:
            if command_size < 24:
                raise ParseError("truncated dylib load command")
            name_offset = _u32le(data, cursor + 8)
            if name_offset >= command_size:
                raise ParseError("dylib install-name offset is outside its command")
            name = _cstring(
                data,
                cursor + name_offset,
                cursor + command_size,
                "dylib install name",
            )
            dependencies.append(
                Dependency(len(dependencies) + 1, name, kind)
            )
        elif command == LC_DYLD_CHAINED_FIXUPS:
            if command_size < 16:
                raise ParseError("truncated LC_DYLD_CHAINED_FIXUPS command")
            if fixups is not None:
                raise ParseError("multiple LC_DYLD_CHAINED_FIXUPS commands are unsupported")
            fixups = (_u32le(data, cursor + 8), _u32le(data, cursor + 12))

        cursor += command_size

    if cursor != commands_end:
        # dyld permits command padding only inside individual cmdsize values;
        # an ncmds/sizeofcmds mismatch is diagnostic evidence of malformed input.
        raise ParseError("Mach-O ncmds/sizeofcmds do not describe the same command region")

    return dependencies, fixups


def _resolve_provider(ordinal: int, dependencies: Sequence[Dependency]) -> tuple[str | None, str]:
    if ordinal > 0:
        if ordinal > len(dependencies):
            raise ParseError(
                f"positive import ordinal {ordinal} exceeds dependency count {len(dependencies)}"
            )
        return dependencies[ordinal - 1].install_name, "dependency"
    return None, _SPECIAL_ORDINALS.get(ordinal, f"special({ordinal})")


def _parse_chained_imports(
    data: bytes,
    slice_offset: int,
    slice_size: int,
    fixups: tuple[int, int] | None,
    dependencies: Sequence[Dependency],
) -> tuple[list[ImportBinding], int | None]:
    if fixups is None:
        return [], None

    data_offset, data_size = fixups
    if data_size == 0:
        raise ParseError("LC_DYLD_CHAINED_FIXUPS has an empty payload")
    if data_offset > slice_size or data_size > slice_size - data_offset:
        raise ParseError("chained-fixup payload exceeds the ARM64 slice")

    begin = slice_offset + data_offset
    _require_range(len(data), begin, data_size, "chained-fixup payload")
    if data_size < 28:
        raise ParseError("chained-fixup header is truncated")

    (
        fixups_version,
        _starts_offset,
        imports_offset,
        symbols_offset,
        imports_count,
        imports_format,
        symbols_format,
    ) = struct.unpack_from("<7I", data, begin)

    if fixups_version != 0:
        raise ParseError(f"unsupported chained-fixups version {fixups_version}")
    if symbols_format != 0:
        raise ParseError(
            f"unsupported chained-fixup symbols format {symbols_format}; "
            "only the uncompressed symbol pool is currently understood"
        )

    entry_size = {
        DYLD_CHAINED_IMPORT: 4,
        DYLD_CHAINED_IMPORT_ADDEND: 8,
        DYLD_CHAINED_IMPORT_ADDEND64: 16,
    }.get(imports_format)
    if entry_size is None:
        raise ParseError(f"unknown chained-import format {imports_format}")

    if imports_offset > data_size or symbols_offset > data_size:
        raise ParseError("chained-fixup table offsets exceed the payload")
    if imports_count > (data_size - imports_offset) // entry_size:
        raise ParseError("chained-import table exceeds the payload")

    imports_begin = begin + imports_offset
    symbols_begin = begin + symbols_offset
    payload_end = begin + data_size
    bindings: list[ImportBinding] = []

    for index in range(imports_count):
        entry = imports_begin + index * entry_size
        if imports_format in (DYLD_CHAINED_IMPORT, DYLD_CHAINED_IMPORT_ADDEND):
            raw = _u32le(data, entry)
            ordinal = _sign_extend(raw & 0xFF, 8)
            weak = bool((raw >> 8) & 1)
            name_offset = raw >> 9
            addend = (
                struct.unpack_from("<i", data, entry + 4)[0]
                if imports_format == DYLD_CHAINED_IMPORT_ADDEND
                else 0
            )
        else:
            raw = _u64le(data, entry)
            ordinal = _sign_extend(raw & 0xFFFF, 16)
            weak = bool((raw >> 16) & 1)
            name_offset = raw >> 32
            addend = struct.unpack_from("<q", data, entry + 8)[0]

        if name_offset >= data_size - symbols_offset:
            raise ParseError("chained-import symbol name offset exceeds the symbol pool")
        symbol = _cstring(
            data,
            symbols_begin + name_offset,
            payload_end,
            "chained-import symbol",
        )
        provider, provider_kind = _resolve_provider(ordinal, dependencies)
        bindings.append(
            ImportBinding(symbol, ordinal, provider, provider_kind, weak, addend)
        )

    return bindings, imports_format


def analyze_bytes(data: bytes, image_name: str) -> ImageSurface:
    """Analyze one Mach-O byte sequence and return a deterministic surface."""
    slice_offset, slice_size = _select_arm64_slice(data)
    dependencies, fixups = _parse_dependencies_and_fixups(
        data, slice_offset, slice_size
    )
    imports, import_format = _parse_chained_imports(
        data, slice_offset, slice_size, fixups, dependencies
    )

    # Preserve dependency ordinals exactly. Sort imports only for the manifest so
    # repeated runs are stable even if upstream generation order changes.
    imports.sort(
        key=lambda item: (
            item.provider or "",
            item.ordinal,
            item.symbol,
            item.weak,
            item.addend,
        )
    )
    return ImageSurface(
        image=image_name.replace("\\", "/"),
        dependencies=tuple(dependencies),
        imports=tuple(imports),
        chained_fixups=fixups is not None,
        chained_import_format=import_format,
    )


def analyze_file(path: Path, display_name: str | None = None) -> ImageSurface:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ParseError(f"could not read {display_name or path.name}: {exc.strerror or exc}") from exc
    return analyze_bytes(data, display_name or path.name)


def build_manifest(images: Iterable[ImageSurface]) -> dict:
    ordered = sorted(images, key=lambda image: image.image)
    provider_counts: dict[str, int] = {}
    dependency_count = 0
    import_count = 0
    weak_count = 0
    special_count = 0

    rendered_images = []
    for image in ordered:
        dependency_count += len(image.dependencies)
        import_count += len(image.imports)
        weak_count += sum(1 for item in image.imports if item.weak)
        special_count += sum(1 for item in image.imports if item.ordinal <= 0)
        for item in image.imports:
            key = item.provider if item.provider is not None else item.provider_kind
            provider_counts[key] = provider_counts.get(key, 0) + 1

        rendered_images.append(
            {
                "image": image.image,
                "chained_fixups": image.chained_fixups,
                "chained_import_format": image.chained_import_format,
                "dependencies": [asdict(item) for item in image.dependencies],
                "imports": [asdict(item) for item in image.imports],
            }
        )

    return {
        "schema_version": 1,
        "summary": {
            "image_count": len(ordered),
            "dependency_count": dependency_count,
            "import_count": import_count,
            "weak_import_count": weak_count,
            "special_ordinal_import_count": special_count,
            "providers": [
                {"provider": provider, "import_count": count}
                for provider, count in sorted(provider_counts.items())
            ],
        },
        "images": rendered_images,
    }


def _resolve_inputs(args: argparse.Namespace) -> list[tuple[Path, str]]:
    resolved: list[tuple[Path, str]] = []
    runtime_root = Path(args.runtime_root).resolve() if args.runtime_root else None

    if runtime_root is not None:
        relative_images = args.relative_image or list(_DEFAULT_SIMULATOR_IMAGES)
        for relative in relative_images:
            relative_path = Path(relative)
            if relative_path.is_absolute() or ".." in relative_path.parts:
                raise ParseError("--relative-image must stay inside --runtime-root")
            resolved.append(
                (
                    runtime_root / relative_path,
                    relative_path.as_posix(),
                )
            )

    for raw in args.images:
        path = Path(raw)
        # Never put an absolute local path into a manifest. Direct inputs use
        # their basename; runtime-root inputs retain only the relative path.
        resolved.append((path, path.name))

    if not resolved:
        raise ParseError(
            "no images selected; provide image paths or --runtime-root"
        )
    return resolved


def _parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Inspect ARM64 Mach-O dependencies and chained imports and emit a "
            "deterministic compatibility-surface manifest."
        )
    )
    parser.add_argument("images", nargs="*", help="Mach-O images to inspect")
    parser.add_argument(
        "--runtime-root",
        help=(
            "RuntimeRoot base. With no --relative-image values, the three "
            "simulator libSystem companion images are analyzed."
        ),
    )
    parser.add_argument(
        "--relative-image",
        action="append",
        help="image path relative to --runtime-root; may be repeated",
    )
    parser.add_argument(
        "--output",
        help="write JSON to this file instead of stdout",
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="emit compact JSON rather than indented JSON",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        inputs = _resolve_inputs(args)
        surfaces = [analyze_file(path, display) for path, display in inputs]
        manifest = build_manifest(surfaces)
    except ParseError as exc:
        print(f"[compat-surface] ERROR: {exc}", file=sys.stderr)
        return 2

    text = json.dumps(
        manifest,
        indent=None if args.compact else 2,
        separators=(",", ":") if args.compact else None,
        sort_keys=False,
    )
    if not args.compact:
        text += "\n"

    if args.output:
        try:
            Path(args.output).write_text(text, encoding="utf-8")
        except OSError as exc:
            print(
                f"[compat-surface] ERROR: could not write output: {exc}",
                file=sys.stderr,
            )
            return 2
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
