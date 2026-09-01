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

    def test_wrapper_does_not_reenter_unguarded_transitive_leaf(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            headers = self._headers(sdk_root)
            umbrella = headers / "Inner.h"
            owner = headers / "Sparse" / "Solve.h"
            leaf = headers / "Sparse" / "SolveImplementation.h"

            # Use quoted SDK-authored edges here so the generated wrapper itself can
            # represent a completely self-contained synthetic translation unit. The
            # implementation leaf intentionally has no include guard, matching the
            # class of SDK headers that exposed the duplicate-entry regression.
            umbrella.write_text(
                '#include "Sparse/Solve.h"\n',
                encoding="utf-8",
            )
            owner.write_text(
                "#ifndef INNER_SOLVE_OWNER\n"
                "#define INNER_SOLVE_OWNER\n"
                "#define INNER_SOLVE_CONTEXT 1\n"
                '#include "SolveImplementation.h"\n'
                "#endif\n",
                encoding="utf-8",
            )
            leaf.write_text(
                "#ifndef INNER_SOLVE_CONTEXT\n"
                "#error Do not include this header directly.\n"
                "#endif\n"
                "enum InnerSolveMethod { InnerSolveDefault = 0 };\n"
                "int InnerSolve(void);\n",
                encoding="utf-8",
            )

            relative = leaf.resolve().relative_to(sdk_root.resolve()).as_posix()
            wrapper_root = Path(directory) / "wrappers"
            sdk_abi_context._write_wrapper(
                wrapper_root,
                relative=relative,
                source=leaf,
                sdk_root=sdk_root,
            )

            wrapper = wrapper_root / relative
            text = wrapper.read_text(encoding="utf-8")
            umbrella_include = f'#include "{umbrella.resolve().as_posix()}"'
            leaf_include = f'#include "{leaf.resolve().as_posix()}"'
            self.assertIn(umbrella_include, text)
            self.assertNotIn(leaf_include, text)


if __name__ == "__main__":
    unittest.main()
