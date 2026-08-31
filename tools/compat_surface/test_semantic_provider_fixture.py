import json
import pathlib
import re
import unittest

import runtime_adapter_table


FIXTURES = pathlib.Path(__file__).with_name("fixtures")
ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCTION_ADAPTERS = (
    ROOT / "src" / "IpaSimulator" / "GeneratedSemanticProviderAdapters.inc"
)
SYS_TRANSLATOR = ROOT / "src" / "IpaSimulator" / "SysTranslator.cpp"


class SemanticProviderFixtureTests(unittest.TestCase):
    def _generated_table(self):
        plan_path = FIXTURES / "semantic_provider_bridge_plan.json"
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        table = runtime_adapter_table.build_runtime_adapter_table(plan)

        self.assertEqual(table["summary"]["adapter_count"], 1)
        self.assertEqual(table["summary"]["ready_count"], 1)
        self.assertEqual(table["summary"]["boundary_count"], 0)
        self.assertEqual(table["adapters"][0]["symbol"], "_getpid")
        self.assertFalse(table["adapters"][0]["requires_pointer_validation"])
        return table

    def test_generated_cpp_fixture_has_no_drift(self):
        table = self._generated_table()
        expected_path = FIXTURES / "semantic_provider_fixture.inc"

        rendered = runtime_adapter_table.render_cpp_table(
            table,
            "makeGeneratedSemanticProviderFixture",
        )
        self.assertEqual(
            expected_path.read_text(encoding="utf-8"),
            rendered,
            "semantic provider C++ fixture drifted from runtime_adapter_table.py",
        )

    def test_production_generated_adapter_has_no_drift(self):
        table = self._generated_table()
        rendered = runtime_adapter_table.render_cpp_table(
            table,
            "makeGeneratedSemanticProviderAdapters",
        )
        self.assertEqual(
            PRODUCTION_ADAPTERS.read_text(encoding="utf-8"),
            rendered,
            "production semantic adapter drifted from runtime_adapter_table.py",
        )

    def test_getpid_is_not_duplicated_in_handwritten_darwin_abi_table(self):
        table = self._generated_table()
        self.assertEqual(table["adapters"][0]["symbol"], "_getpid")

        translator = SYS_TRANSLATOR.read_text(encoding="utf-8")
        handwritten_getpid = re.compile(r'\{\s*"getpid"\s*,\s*\d+\s*,')
        self.assertIsNone(
            handwritten_getpid.search(translator),
            "_getpid is generated and must not also exist in the handwritten Darwin ABI table",
        )
        self.assertIn(
            "isSelectedGeneratedSemanticImport(Addr)",
            translator,
            "SysTranslator no longer checks the loader-selected generated route",
        )


if __name__ == "__main__":
    unittest.main()
