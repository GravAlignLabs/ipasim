import tempfile
import unittest
from pathlib import Path

import sdk_abi_context


class SdkAbiNestedFrameworkContextTests(unittest.TestCase):
    def _headers(self, root: Path) -> Path:
        headers = (
            root
            / "System"
            / "Library"
            / "Frameworks"
            / "Outer.framework"
            / "Frameworks"
            / "Inner.framework"
            / "Headers"
        )
        (headers / "Sparse").mkdir(parents=True)
        return headers

    def test_nested_framework_umbrella_can_reach_leaf_transitively(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            headers = self._headers(sdk_root)
            umbrella = headers / "Inner.h"
            owner = headers / "Sparse" / "Solve.h"
            leaf = headers / "Sparse" / "SolveImplementation.h"

            umbrella.write_text(
                "#include <Inner/Sparse/Solve.h>\n",
                encoding="utf-8",
            )
            owner.write_text(
                "#define INNER_SOLVE_CONTEXT 1\n"
                "#include <Inner/Sparse/SolveImplementation.h>\n",
                encoding="utf-8",
            )
            leaf.write_text(
                "#ifndef INNER_SOLVE_CONTEXT\n"
                "#error Do not include this header directly.\n"
                "#endif\n"
                "int InnerSolve(void);\n",
                encoding="utf-8",
            )

            self.assertEqual(
                sdk_abi_context.recommended_preludes(
                    leaf,
                    sdk_root=sdk_root,
                ),
                [umbrella.resolve()],
            )

    def test_nested_framework_umbrella_must_prove_reachability(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            headers = self._headers(sdk_root)
            umbrella = headers / "Inner.h"
            leaf = headers / "Sparse" / "UnownedImplementation.h"
            umbrella.write_text(
                "#include <Inner/Sparse/Other.h>\n",
                encoding="utf-8",
            )
            (headers / "Sparse" / "Other.h").write_text(
                "int Other(void);\n",
                encoding="utf-8",
            )
            leaf.write_text(
                "#error Do not include this header directly.\n",
                encoding="utf-8",
            )

            self.assertEqual(
                sdk_abi_context.recommended_preludes(
                    leaf,
                    sdk_root=sdk_root,
                ),
                [],
            )


if __name__ == "__main__":
    unittest.main()
