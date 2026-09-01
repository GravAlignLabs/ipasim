import shutil
import tempfile
import unittest
from pathlib import Path

import sdk_header_exhaustive


class SdkHeaderSupportReachabilityTests(unittest.TestCase):
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

    def test_independently_compilable_support_leaf_is_inactive_when_owner_does_not_enter_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/c++/v1/__support/other/xlocale.h",
                "// -*- C++ -*-\n"
                "#ifndef SYNTHETIC_SUPPORT_XLOCALE_H\n"
                "#define SYNTHETIC_SUPPORT_XLOCALE_H\n"
                'extern "C" long long support_only_value(const int *value);\n'
                "#endif\n",
            )
            self.write(
                root,
                "usr/include/c++/v1/__locale",
                "// -*- C++ -*-\n"
                "#ifdef SYNTHETIC_OTHER_PLATFORM\n"
                "#include <__support/other/xlocale.h>\n"
                "#endif\n",
            )
            display = "usr/include/c++/v1/__support/other/xlocale.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )

            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            inactive = manifest["target_inactive_headers"][0]
            self.assertEqual(inactive["header"], display)
            self.assertEqual(
                inactive["context_header"],
                "usr/include/c++/v1/__locale",
            )
            self.assertIn("no compiling SDK owner activates it", inactive["reason"])

    def test_support_leaf_contributes_when_compiling_owner_enters_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/c++/v1/__support/platform/api.h",
                "// -*- C++ -*-\n"
                "#ifndef SYNTHETIC_SUPPORT_API_H\n"
                "#define SYNTHETIC_SUPPORT_API_H\n"
                'extern "C" int support_api(int value);\n'
                "#endif\n",
            )
            self.write(
                root,
                "usr/include/c++/v1/__locale",
                "// -*- C++ -*-\n"
                "#include <__support/platform/api.h>\n",
            )
            display = "usr/include/c++/v1/__support/platform/api.h"

            manifest = sdk_header_exhaustive.build_parallel_manifest(
                [(leaf, display)], jobs=1, sdk_root=root
            )

            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_support_api"],
            )
            self.assertNotIn("target_inactive_headers", manifest)


if __name__ == "__main__":
    unittest.main()
