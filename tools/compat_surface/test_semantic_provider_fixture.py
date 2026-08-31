import json
import pathlib
import unittest

import runtime_adapter_table


FIXTURES = pathlib.Path(__file__).with_name("fixtures")
ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCTION_ADAPTERS = (
    ROOT / "src" / "IpaSimulator" / "GeneratedSemanticProviderAdapters.inc"
)


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


if __name__ == "__main__":
    unittest.main()
