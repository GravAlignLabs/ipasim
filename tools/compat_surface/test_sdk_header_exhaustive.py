import json
import shutil
import tempfile
import unittest
from pathlib import Path

import header_surface
import sdk_header_exhaustive


class SdkHeaderExhaustiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    @staticmethod
    def write(root: Path, relative: str, text: str) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def test_cxx_modeline_selects_objective_cxx(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/lib/swift/shims/CxxShim.h",
                "// probe --------------------------------*- C++ -*-\n"
                "template <class T> struct Box { T value; };\n"
                'extern "C" int cxx_shim_value(int value);\n',
            )
            display = "usr/lib/swift/shims/CxxShim.h"

            with self.assertRaises(header_surface.HeaderParseError):
                header_surface.analyze_header(leaf, display, sdk_root=root)

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_cxx_shim_value"],
            )

    def test_module_guard_enables_clang_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/os/_module_only.h",
                "#if !__has_feature(modules)\n"
                '#error "modules required"\n'
                "#endif\n"
                "extern int module_only_value(int value);\n",
            )
            display = "usr/include/os/_module_only.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_module_only_value"],
            )

    def test_local_extern_inside_inline_function_is_not_global_surface(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/lib/swift/shims/LocalExtern.h",
                "// probe --------------------------------*- C++ -*-\n"
                "static inline int wrapper(int value) {\n"
                "  extern int hidden_local(int);\n"
                "  return hidden_local(value);\n"
                "}\n"
                'extern "C" int visible_global(int value);\n',
            )
            display = "usr/lib/swift/shims/LocalExtern.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )
            encoded = json.dumps(manifest)
            self.assertIn("_visible_global", encoded)
            self.assertNotIn("hidden_local", encoded)

    def test_unknown_type_recovers_from_sdk_definition_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            self.write(
                root,
                "usr/include/db.h",
                "#pragma once\n"
                "typedef unsigned int pgno_t;\n"
                "#define __BEGIN_DECLS\n"
                "#define __END_DECLS\n",
            )
            leaf = self.write(
                root,
                "usr/include/mpool.h",
                "#pragma once\n"
                "__BEGIN_DECLS\n"
                "extern int mpool_probe(pgno_t value);\n"
                "__END_DECLS\n",
            )
            display = "usr/include/mpool.h"

            with self.assertRaises(header_surface.HeaderParseError):
                header_surface.analyze_header(leaf, display, sdk_root=root)

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_mpool_probe"],
            )

    def test_sdk_authored_error_is_explicit_target_inactive_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/unsupported.h",
                "#error synthetic header unsupported\n",
            )
            display = "usr/include/unsupported.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )
            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            self.assertIn(
                "explicitly rejects",
                manifest["target_inactive_headers"][0]["reason"],
            )

    def test_absent_sdk_dependency_is_explicit_target_inactive_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/mach/incomplete.h",
                "#include <mach/not_shipped.h>\n"
                "extern int unreachable_api(void);\n",
            )
            display = "usr/include/mach/incomplete.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )
            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            self.assertIn(
                "absent from this SDK installation",
                manifest["target_inactive_headers"][0]["reason"],
            )


if __name__ == "__main__":
    unittest.main()
