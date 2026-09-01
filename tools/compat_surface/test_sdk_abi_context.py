import shutil
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import sdk_abi_context


class SdkAbiContextTests(unittest.TestCase):
    def _write_apple_archive_fixture(self, root: Path) -> tuple[Path, Path]:
        directory = root / "usr" / "include" / "AppleArchive"
        directory.mkdir(parents=True)
        umbrella = directory / "AppleArchive.h"
        leaf = directory / "AAArchiveStream.h"
        umbrella.write_text(
            "#ifndef TEST_APPLE_ARCHIVE_H\n"
            "#define TEST_APPLE_ARCHIVE_H\n"
            "#define APPLE_ARCHIVE_API __attribute__((visibility(\"default\")))\n"
            "typedef struct AAArchiveStream_impl *AAArchiveStream;\n"
            "#include \"AAArchiveStream.h\"\n"
            "#endif\n",
            encoding="utf-8",
        )
        leaf.write_text(
            "#ifndef TEST_AA_ARCHIVE_STREAM_H\n"
            "#define TEST_AA_ARCHIVE_STREAM_H\n"
            "#ifndef TEST_APPLE_ARCHIVE_H\n"
            "#error Include AppleArchive.h instead of this file\n"
            "#endif\n"
            "APPLE_ARCHIVE_API void AAArchiveStreamCancel(AAArchiveStream s);\n"
            "#endif\n",
            encoding="utf-8",
        )
        return umbrella, leaf

    def test_recommended_preludes_resolves_sdk_named_sibling_umbrella(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            umbrella, leaf = self._write_apple_archive_fixture(sdk_root)
            self.assertEqual(
                sdk_abi_context.recommended_preludes(leaf, sdk_root=sdk_root),
                [umbrella.resolve()],
            )

    def test_aapcs64_wrapper_includes_umbrella_before_physical_leaf(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            umbrella, leaf = self._write_apple_archive_fixture(sdk_root)
            relative = "usr/include/AppleArchive/AAArchiveStream.h"
            selected = [
                SimpleNamespace(
                    source_header=relative,
                    c_name="AAArchiveStreamCancel",
                )
            ]
            captured = {}

            def fake_build(inventory, **kwargs):
                wrapper_root = Path(kwargs["header_root"])
                wrapper = wrapper_root / relative
                captured["text"] = wrapper.read_text(encoding="utf-8")
                captured["root"] = wrapper_root
                return {"kind": "aapcs64-abi-surface"}

            with (
                mock.patch.object(
                    sdk_abi_context.abi_surface,
                    "_validate_inventory",
                    return_value=(None, selected),
                ),
                mock.patch.object(
                    sdk_abi_context.compiler_batching,
                    "build_aapcs64_manifest",
                    side_effect=fake_build,
                ) as build,
            ):
                result = sdk_abi_context.build_aapcs64_manifest(
                    {"symbols": []},
                    header_root=sdk_root,
                    sdk_root=sdk_root,
                    clang="clang",
                    extra_args=("-DTEST",),
                    timeout_seconds=17,
                    batch_size=23,
                )

            self.assertEqual(result, {"kind": "aapcs64-abi-surface"})
            text = captured["text"]
            umbrella_include = f'#include "{umbrella.resolve().as_posix()}"'
            leaf_include = f'#include "{leaf.resolve().as_posix()}"'
            self.assertIn(umbrella_include, text)
            self.assertIn(leaf_include, text)
            self.assertLess(text.index(umbrella_include), text.index(leaf_include))
            self.assertIn("#ifdef AAArchiveStreamCancel", text)
            self.assertIn("#undef AAArchiveStreamCancel", text)
            self.assertLess(
                text.index(leaf_include),
                text.index("#undef AAArchiveStreamCancel"),
            )
            build.assert_called_once()
            kwargs = build.call_args.kwargs
            self.assertEqual(kwargs["sdk_root"], sdk_root.resolve())
            self.assertEqual(kwargs["batch_size"], 23)
            self.assertEqual(kwargs["extra_args"], ("-DTEST",))
            self.assertEqual(kwargs["timeout_seconds"], 17)

    @unittest.skipUnless(shutil.which("clang"), "clang is required")
    def test_exported_function_macro_alias_does_not_replace_abi_probe_target(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            header = sdk_root / "usr" / "include" / "MacroAlias.h"
            header.parent.mkdir(parents=True)
            header.write_text(
                "int PublicThing(int value);\n"
                "static inline int __PublicThing(int value) { return value; }\n"
                "#define PublicThing __PublicThing\n",
                encoding="utf-8",
            )
            relative = "usr/include/MacroAlias.h"
            int_type = {"kind": "builtin", "name": "int"}
            signature = {
                "symbol": "_PublicThing",
                "names": ["PublicThing"],
                "function_type_spellings": [],
                "calling_convention": "cdecl",
                "variadic": False,
                "prototype": True,
                "return_type": int_type,
                "parameters": [
                    {
                        "index": 0,
                        "names": ["value"],
                        "spellings": ["int"],
                        "type": int_type,
                    }
                ],
                "sources": [
                    {
                        "header": relative,
                        "line": 1,
                        "column": 1,
                    }
                ],
            }
            inventory = {
                "schema_version": 1,
                "kind": "typed-compatibility-inventory",
                "targets": {
                    "clang": "arm64-apple-ios16.0",
                    "tapi": "arm64-ios",
                },
                "summary": {},
                "symbols": [
                    {
                        "symbol": "_PublicThing",
                        "required_by": [],
                        "requirement_count": 0,
                        "sdk_direct_exports": [],
                        "signature": signature,
                    }
                ],
                "requirements": [],
            }

            manifest = sdk_abi_context.build_aapcs64_manifest(
                inventory,
                header_root=sdk_root,
                sdk_root=sdk_root,
                clang="clang",
                timeout_seconds=30,
                batch_size=8,
            )

            self.assertEqual(manifest["summary"]["typed_symbol_count"], 1)
            self.assertEqual(manifest["symbols"][0]["symbol"], "_PublicThing")
            self.assertEqual(manifest["symbols"][0]["c_name"], "PublicThing")
            self.assertEqual(manifest["symbols"][0]["llvm_ir_name"], "PublicThing")

    def test_without_sdk_root_delegates_without_context_wrappers(self):
        expected = {"kind": "aapcs64-abi-surface"}
        with mock.patch.object(
            sdk_abi_context.compiler_batching,
            "build_aapcs64_manifest",
            return_value=expected,
        ) as build:
            actual = sdk_abi_context.build_aapcs64_manifest(
                {"symbols": []},
                header_root=Path("headers"),
                sdk_root=None,
                clang="clang",
                extra_args=("-DTEST",),
                timeout_seconds=11,
                batch_size=7,
            )
        self.assertIs(actual, expected)
        build.assert_called_once_with(
            {"symbols": []},
            header_root=Path("headers"),
            sdk_root=None,
            batch_size=7,
            clang="clang",
            extra_args=("-DTEST",),
            timeout_seconds=11,
        )


if __name__ == "__main__":
    unittest.main()
