import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import sdk_abi_context


class SdkAbiRecoveredUsrContextsTests(unittest.TestCase):
    def test_usr_package_umbrella_proves_rpc_style_prerequisites(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            rpc = sdk_root / "usr" / "include" / "rpc"
            rpc.mkdir(parents=True)
            umbrella = rpc / "rpc.h"
            types = rpc / "types.h"
            leaf = rpc / "rpc_msg.h"

            umbrella.write_text(
                "#ifndef TEST_RPC_H\n"
                "#define TEST_RPC_H\n"
                "#include <rpc/types.h>\n"
                "#include <rpc/rpc_msg.h>\n"
                "#endif\n",
                encoding="utf-8",
            )
            types.write_text("typedef int rpc_word_t;\n", encoding="utf-8")
            leaf.write_text(
                "#ifndef TEST_RPC_MSG_H\n"
                "#define TEST_RPC_MSG_H\n"
                "rpc_word_t rpc_message_word(void);\n"
                "#endif\n",
                encoding="utf-8",
            )

            self.assertEqual(
                sdk_abi_context.recommended_preludes(leaf, sdk_root=sdk_root),
                [umbrella.resolve()],
            )

    def test_non_libcxx_recommendation_prefers_ordinary_usr_header(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            usr_include = sdk_root / "usr" / "include"
            libcxx = usr_include / "c++" / "v1"
            secure = usr_include / "secure"
            libcxx.mkdir(parents=True)
            secure.mkdir(parents=True)

            ordinary = usr_include / "stdio.h"
            ordinary.write_text(
                "#ifndef TEST_STDIO_H\n#define TEST_STDIO_H\n#endif\n",
                encoding="utf-8",
            )
            (libcxx / "stdio.h").write_text(
                "#include <__config>\n",
                encoding="utf-8",
            )
            leaf = secure / "_stdio.h"
            leaf.write_text(
                '#ifndef TEST_STDIO_H\n'
                '#error "Never use <secure/_stdio.h> directly; include <stdio.h> instead."\n'
                "#endif\n",
                encoding="utf-8",
            )

            self.assertEqual(
                sdk_abi_context.recommended_preludes(leaf, sdk_root=sdk_root),
                [ordinary.resolve()],
            )

    def test_swift_importer_define_is_scoped_around_wrapper_source(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            shims = sdk_root / "usr" / "lib" / "swift" / "shims"
            shims.mkdir(parents=True)
            source = shims / "HeapObject.h"
            source.write_text(
                "#ifndef TEST_SWIFT_HEAP_OBJECT_H\n"
                "#define TEST_SWIFT_HEAP_OBJECT_H\n"
                "#ifndef __swift__\n"
                "#error importer context required\n"
                "#endif\n"
                "int SwiftShimThing(void);\n"
                "#endif\n",
                encoding="utf-8",
            )
            relative = source.relative_to(sdk_root).as_posix()
            wrapper_root = Path(directory) / "wrappers"

            sdk_abi_context._write_wrapper(
                wrapper_root,
                relative=relative,
                source=source,
                sdk_root=sdk_root,
                selected_c_names=["SwiftShimThing"],
            )

            text = (wrapper_root / relative).read_text(encoding="utf-8")
            define = "#define __swift__ 1"
            source_include = f'#include "{source.resolve().as_posix()}"'
            undef = "#undef __swift__"
            self.assertIn(define, text)
            self.assertIn(source_include, text)
            self.assertIn(undef, text)
            self.assertLess(text.index(define), text.index(source_include))
            self.assertLess(text.index(source_include), text.index(undef))

    def test_aapcs64_adds_libcxx_root_with_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            libcxx = sdk_root / "usr" / "include" / "c++" / "v1"
            source = sdk_root / "usr" / "include" / "Example.h"
            libcxx.mkdir(parents=True)
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("int Example(void);\n", encoding="utf-8")
            selected = [
                SimpleNamespace(
                    source_header="usr/include/Example.h",
                    c_name="Example",
                )
            ]
            captured = {}

            def fake_build(inventory, **kwargs):
                captured["extra_args"] = kwargs["extra_args"]
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
                ),
            ):
                sdk_abi_context.build_aapcs64_manifest(
                    {"symbols": []},
                    header_root=sdk_root,
                    sdk_root=sdk_root,
                    extra_args=("-DTEST=1",),
                )

            args = captured["extra_args"]
            self.assertEqual(args[0], "-DTEST=1")
            self.assertIn("-I", args)
            index = args.index("-I")
            self.assertEqual(args[index + 1], str(libcxx.resolve()))


if __name__ == "__main__":
    unittest.main()
