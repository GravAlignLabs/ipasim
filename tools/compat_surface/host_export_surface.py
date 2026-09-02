#!/usr/bin/env python3
"""Parse an explicit Windows PE .def export surface for compatibility planning.

This parser records export identity only. A PE export existing in a host bridge is
candidate evidence for migration planning; it is never semantic approval and it
never proves that the export is callable from an iOS guest.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence


class HostExportSurfaceError(ValueError):
    """Raised when a PE .def export surface is malformed or ambiguous."""


def _strip_comment(line: str) -> str:
    return line.split(";", 1)[0].strip()


def _library_filename(name: str) -> str:
    return name if name.casefold().endswith(".dll") else name + ".dll"


def parse_def_text(text: str, *, source: str = "<memory>") -> dict:
    library: str | None = None
    in_exports = False
    exports: list[dict] = []
    seen: set[str] = set()

    for line_number, raw in enumerate(text.splitlines(), start=1):
        line = _strip_comment(raw)
        if not line:
            continue
        upper = line.upper()

        if upper.startswith("LIBRARY"):
            if in_exports:
                raise HostExportSurfaceError(
                    f"{source}:{line_number}: LIBRARY appears after EXPORTS"
                )
            rest = line[len("LIBRARY") :].strip()
            if not rest:
                raise HostExportSurfaceError(
                    f"{source}:{line_number}: LIBRARY requires a name"
                )
            if library is not None:
                raise HostExportSurfaceError(
                    f"{source}:{line_number}: duplicate LIBRARY declaration"
                )
            if rest.startswith('"') and rest.endswith('"') and len(rest) >= 2:
                rest = rest[1:-1]
            if not rest or any(char.isspace() for char in rest):
                raise HostExportSurfaceError(
                    f"{source}:{line_number}: unsupported LIBRARY name {rest!r}"
                )
            library = rest
            continue

        if upper == "EXPORTS":
            if in_exports:
                raise HostExportSurfaceError(
                    f"{source}:{line_number}: duplicate EXPORTS section"
                )
            in_exports = True
            continue

        if not in_exports:
            raise HostExportSurfaceError(
                f"{source}:{line_number}: unsupported directive before EXPORTS: {line!r}"
            )

        tokens = line.split()
        identity = tokens[0]
        attributes = tokens[1:]
        if "=" in identity:
            export_name, target = identity.split("=", 1)
        else:
            export_name = identity
            target = identity
        if not export_name or not target:
            raise HostExportSurfaceError(
                f"{source}:{line_number}: invalid export identity {identity!r}"
            )
        if export_name in seen:
            raise HostExportSurfaceError(
                f"{source}:{line_number}: duplicate export {export_name!r}"
            )
        seen.add(export_name)

        kind = "function"
        ordinal: int | None = None
        noname = False
        private = False
        for attribute in attributes:
            folded = attribute.upper()
            if folded == "DATA":
                kind = "data"
            elif folded == "NONAME":
                noname = True
            elif folded == "PRIVATE":
                private = True
            elif attribute.startswith("@"):
                if ordinal is not None:
                    raise HostExportSurfaceError(
                        f"{source}:{line_number}: duplicate export ordinal"
                    )
                try:
                    ordinal = int(attribute[1:])
                except ValueError as exc:
                    raise HostExportSurfaceError(
                        f"{source}:{line_number}: invalid export ordinal {attribute!r}"
                    ) from exc
                if ordinal <= 0:
                    raise HostExportSurfaceError(
                        f"{source}:{line_number}: export ordinal must be positive"
                    )
            else:
                raise HostExportSurfaceError(
                    f"{source}:{line_number}: unsupported export attribute {attribute!r}"
                )

        item = {
            "name": export_name,
            "target": target,
            "kind": kind,
        }
        if ordinal is not None:
            item["ordinal"] = ordinal
        if noname:
            item["noname"] = True
        if private:
            item["private"] = True
        exports.append(item)

    if library is None:
        raise HostExportSurfaceError(f"{source}: missing LIBRARY declaration")
    if not in_exports:
        raise HostExportSurfaceError(f"{source}: missing EXPORTS section")
    if not exports:
        raise HostExportSurfaceError(f"{source}: EXPORTS section is empty")

    exports.sort(key=lambda item: item["name"])
    return {
        "schema_version": 1,
        "kind": "pe-def-export-surface",
        "library": library,
        "library_filename": _library_filename(library),
        "summary": {
            "export_count": len(exports),
            "function_export_count": sum(item["kind"] == "function" for item in exports),
            "data_export_count": sum(item["kind"] == "data" for item in exports),
        },
        "exports": exports,
    }


def parse_def_file(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise HostExportSurfaceError(
            f"could not read PE .def file {path.name}: {exc.strerror or exc}"
        ) from exc
    return parse_def_text(text, source=path.name)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--def-file", required=True)
    parser.add_argument("--output")
    args = parser.parse_args(argv)

    try:
        manifest = parse_def_file(Path(args.def_file))
        rendered = json.dumps(manifest, indent=2, sort_keys=False) + "\n"
        if args.output:
            Path(args.output).write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0
    except (HostExportSurfaceError, OSError) as exc:
        print(f"[host-export-surface] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
