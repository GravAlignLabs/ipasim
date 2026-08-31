import json
import struct
import tempfile
import unittest
from pathlib import Path

import compat_surface as cs


def _align(value, alignment=8):
    return (value + alignment - 1) & ~(alignment - 1)


def _dylib_command(command, name):
    encoded = name.encode("utf-8") + b"\0"
    command_size = _align(24 + len(encoded))
    payload = struct.pack(
        "<IIIIII",
        command,
        command_size,
        24,
        0,
        0,
        0,
    )
    return payload + encoded + b"\0" * (command_size - len(payload) - len(encoded))


def _encode_import(fmt, ordinal, weak, name_offset, addend):
    if fmt in (cs.DYLD_CHAINED_IMPORT, cs.DYLD_CHAINED_IMPORT_ADDEND):
        raw = (ordinal & 0xFF) | (int(weak) << 8) | (name_offset << 9)
        entry = struct.pack("<I", raw)
        if fmt == cs.DYLD_CHAINED_IMPORT_ADDEND:
            entry += struct.pack("<i", addend)
        return entry
    if fmt == cs.DYLD_CHAINED_IMPORT_ADDEND64:
        raw = (ordinal & 0xFFFF) | (int(weak) << 16) | (name_offset << 32)
        return struct.pack("<Qq", raw, addend)
    raise AssertionError(fmt)


def build_thin(
    dependencies,
    imports,
    import_format=cs.DYLD_CHAINED_IMPORT,
):
    dep_commands = [
        _dylib_command(command, name)
        for command, name in dependencies
    ]

    symbol_pool = bytearray()
    import_entries = bytearray()
    for symbol, ordinal, weak, addend in imports:
        name_offset = len(symbol_pool)
        symbol_pool.extend(symbol.encode("utf-8") + b"\0")
        import_entries.extend(
            _encode_import(
                import_format, ordinal, weak, name_offset, addend
            )
        )

    imports_offset = 28
    symbols_offset = imports_offset + len(import_entries)
    fixups_payload = (
        struct.pack(
            "<7I",
            0,
            0,
            imports_offset,
            symbols_offset,
            len(imports),
            import_format,
            0,
        )
        + bytes(import_entries)
        + bytes(symbol_pool)
    )

    fixups_command_size = 16
    sizeofcmds = sum(len(item) for item in dep_commands) + fixups_command_size
    dataoff = 32 + sizeofcmds
    fixups_command = struct.pack(
        "<IIII",
        cs.LC_DYLD_CHAINED_FIXUPS,
        fixups_command_size,
        dataoff,
        len(fixups_payload),
    )

    header = struct.pack(
        "<IIIIIIII",
        cs.MH_MAGIC_64,
        cs.CPU_TYPE_ARM64,
        0,
        6,
        len(dep_commands) + 1,
        sizeofcmds,
        0,
        0,
    )
    return header + b"".join(dep_commands) + fixups_command + fixups_payload


def build_fat(arm64_slice):
    other = b"X" * 64
    header_size = 8 + 2 * 20
    x86_offset = _align(header_size, 16)
    arm64_offset = _align(x86_offset + len(other), 16)

    fat = bytearray()
    fat.extend(struct.pack(">II", cs.FAT_MAGIC, 2))
    fat.extend(
        struct.pack(
            ">IIIII",
            0x01000007,
            3,
            x86_offset,
            len(other),
            4,
        )
    )
    fat.extend(
        struct.pack(
            ">IIIII",
            cs.CPU_TYPE_ARM64,
            0,
            arm64_offset,
            len(arm64_slice),
            4,
        )
    )
    fat.extend(b"\0" * (x86_offset - len(fat)))
    fat.extend(other)
    fat.extend(b"\0" * (arm64_offset - len(fat)))
    fat.extend(arm64_slice)
    return bytes(fat)


class CompatSurfaceTests(unittest.TestCase):
    def test_thin_arm64_resolves_positive_ordinals(self):
        data = build_thin(
            [
                (cs.LC_LOAD_DYLIB, "/usr/lib/system/libsystem_kernel.dylib"),
                (cs.LC_LOAD_WEAK_DYLIB, "/usr/lib/libobjc.A.dylib"),
            ],
            [
                ("_read", 1, False, 0),
                ("_objc_msgSend", 2, True, 0),
            ],
        )
        surface = cs.analyze_bytes(data, "kernel.dylib")
        self.assertEqual(
            [item.install_name for item in surface.dependencies],
            [
                "/usr/lib/system/libsystem_kernel.dylib",
                "/usr/lib/libobjc.A.dylib",
            ],
        )
        by_symbol = {item.symbol: item for item in surface.imports}
        self.assertEqual(
            by_symbol["_read"].provider,
            "/usr/lib/system/libsystem_kernel.dylib",
        )
        self.assertFalse(by_symbol["_read"].weak)
        self.assertEqual(
            by_symbol["_objc_msgSend"].provider,
            "/usr/lib/libobjc.A.dylib",
        )
        self.assertTrue(by_symbol["_objc_msgSend"].weak)

    def test_addend_format_preserves_signed_special_ordinal_and_addend(self):
        data = build_thin(
            [(cs.LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib")],
            [("_main_symbol", -1, False, -17)],
            cs.DYLD_CHAINED_IMPORT_ADDEND,
        )
        surface = cs.analyze_bytes(data, "addend.dylib")
        item = surface.imports[0]
        self.assertEqual(item.ordinal, -1)
        self.assertEqual(item.provider_kind, "main-executable")
        self.assertIsNone(item.provider)
        self.assertEqual(item.addend, -17)

    def test_fat_arm64_and_addend64(self):
        thin = build_thin(
            [(cs.LC_REEXPORT_DYLIB, "/usr/lib/libSystem.B.dylib")],
            [("_large", 1, False, 0x123456789)],
            cs.DYLD_CHAINED_IMPORT_ADDEND64,
        )
        surface = cs.analyze_bytes(build_fat(thin), "fat.dylib")
        self.assertEqual(surface.chained_import_format, 3)
        self.assertEqual(surface.imports[0].addend, 0x123456789)
        self.assertEqual(surface.dependencies[0].kind, "reexport")

    def test_invalid_positive_ordinal_fails_explicitly(self):
        data = build_thin(
            [(cs.LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib")],
            [("_bad", 2, False, 0)],
        )
        with self.assertRaisesRegex(cs.ParseError, "exceeds dependency count"):
            cs.analyze_bytes(data, "bad.dylib")

    def test_manifest_is_deterministic_and_summarizes_providers(self):
        a = cs.analyze_bytes(
            build_thin(
                [(cs.LC_LOAD_DYLIB, "/usr/lib/A.dylib")],
                [("_z", 1, False, 0), ("_a", 1, True, 0)],
            ),
            "z.dylib",
        )
        b = cs.analyze_bytes(
            build_thin(
                [(cs.LC_LOAD_DYLIB, "/usr/lib/B.dylib")],
                [("_b", 1, False, 0)],
            ),
            "a.dylib",
        )
        manifest1 = cs.build_manifest([a, b])
        manifest2 = cs.build_manifest([b, a])
        self.assertEqual(
            json.dumps(manifest1, separators=(",", ":")),
            json.dumps(manifest2, separators=(",", ":")),
        )
        self.assertEqual(
            [image["image"] for image in manifest1["images"]],
            ["a.dylib", "z.dylib"],
        )
        self.assertEqual(manifest1["summary"]["import_count"], 3)
        self.assertEqual(manifest1["summary"]["weak_import_count"], 1)

    def test_cli_runtime_root_never_emits_absolute_path(self):
        data = build_thin(
            [(cs.LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib")],
            [("_foo", 1, False, 0)],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rel = Path("usr/lib/system/libsystem_sim_kernel.dylib")
            target = root / rel
            target.parent.mkdir(parents=True)
            target.write_bytes(data)
            output = root / "manifest.json"
            exit_code = cs.main(
                [
                    "--runtime-root",
                    str(root),
                    "--relative-image",
                    rel.as_posix(),
                    "--output",
                    str(output),
                ]
            )
            self.assertEqual(exit_code, 0)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["images"][0]["image"], rel.as_posix())
            self.assertNotIn(str(root), output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
