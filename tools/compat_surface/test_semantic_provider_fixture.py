import json
import pathlib
import unittest

import runtime_adapter_table


FIXTURES = pathlib.Path(__file__).with_name("fixtures")


class SemanticProviderFixtureTests(unittest.TestCase):
    def test_generated_cpp_fixture_has_no_drift(self):
        plan_path = FIXTURES / "semantic_provider_bridge_plan.json"
        expected_path = FIXTURES / "semantic_provider_fixture.inc"

        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        table = runtime_adapter_table.build_runtime_adapter_table(plan)

        self.assertEqual(table["summary"]["adapter_count"], 1)
        self.assertEqual(table["summary"]["ready_count"], 1)
        self.assertEqual(table["summary"]["boundary_count"], 0)
        self.assertEqual(table["adapters"][0]["symbol"], "_getpid")
        self.assertFalse(table["adapters"][0]["requires_pointer_validation"])

        rendered = runtime_adapter_table.render_cpp_table(
            table,
            "makeGeneratedSemanticProviderFixture",
        )
        self.assertEqual(
            expected_path.read_text(encoding="utf-8"),
            rendered,
            "semantic provider C++ fixture drifted from runtime_adapter_table.py",
        )


if __name__ == "__main__":
    unittest.main()
