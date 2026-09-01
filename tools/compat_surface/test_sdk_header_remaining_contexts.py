import shutil
import tempfile
import unittest
from pathlib import Path

import sdk_header_exhaustive


class RemainingSdkHeaderContextTests(unittest.TestCase):
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

    def test_swift_shim_uses_sdk_authored_swift_importer_branch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/lib/swift/shims/Probe.h",
                "// -*- C++ -*-\n"
                "#ifdef __swift__\n"
                "extern int swift_importer_probe(int value);\n"
                "#else\n"
                "#include \"swift/Compiler/NotInSdk.h\"\n"
                "#endif\n",
            )
            display = "usr/lib/swift/shims/Probe.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_swift_importer_probe"],
            )
            self.assertNotIn("target_inactive_headers", manifest)

    def test_cxx_modeline_can_fall_back_to_c_for_c_abi_surface(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/lib/swift/shims/Overlay.h",
                "// -*- C++ -*-\n"
                "static inline const unsigned char *overlay_name(void) {\n"
                "  return \"Overlay\";\n"
                "}\n"
                "extern int overlay_probe(int value);\n",
            )
            display = "usr/lib/swift/shims/Overlay.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_overlay_probe"],
            )

    def test_module_guard_prefers_public_owner_without_forcing_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/os/_private.h",
                "#if !__has_feature(modules)\n"
                '#error "Do not include this header directly, please include <os/public.h> instead"\n'
                "#endif\n"
                "extern int private_probe(int value);\n",
            )
            self.write(
                root,
                "usr/include/os/public.h",
                "#pragma once\nextern int public_probe(int value);\n",
            )
            display = "usr/include/os/_private.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            inactive = manifest["target_inactive_headers"][0]
            self.assertEqual(inactive["header"], display)
            self.assertEqual(inactive["context_header"], "usr/include/os/public.h")
            self.assertIn("does not activate", inactive["reason"])

    def test_missing_explicit_module_import_is_recorded_inactive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/lib/swift/shims/MissingOverlay.h",
                "// -*- C++ -*-\n"
                "@import MissingKit;\n"
                "extern int missing_overlay_probe(int value);\n",
            )
            display = "usr/lib/swift/shims/MissingOverlay.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            inactive = manifest["target_inactive_headers"][0]
            self.assertEqual(inactive["header"], display)
            self.assertIn("MissingKit", inactive["reason"])
            self.assertIn("not declared", inactive["reason"])


if __name__ == "__main__":
    unittest.main()
