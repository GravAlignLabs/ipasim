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
APPROVED_ROUTES = (
    ROOT / "src" / "IpaSimulator" / "ApprovedSemanticImportRoutes.inc"
)
ROUTER = ROOT / "src" / "IpaSimulator" / "GeneratedSemanticImportRouter.cpp"
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

    def _approved_routes(self):
        source = APPROVED_ROUTES.read_text(encoding="utf-8")
        pattern = re.compile(
            r"\{\s*"
            r'"(?P<guest>[^"]+)"\s*,\s*'
            r'"(?P<host>[^"]+)"\s*,\s*'
            r'L"(?P<module>[^"]+)"\s*,\s*'
            r'"(?P<adapter>[^"]+)"\s*,\s*'
            r'"(?P<owner>[^"]+)"\s*,\s*'
            r"LiveGuestProfile::(?P<profile>[A-Za-z0-9_]+)\s*,\s*"
            r"\}",
            re.DOTALL,
        )
        return [match.groupdict() for match in pattern.finditer(source)]

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

    def test_approved_route_table_is_explicit_and_backed_by_generated_abi(self):
        table = self._generated_table()
        generated_symbols = {adapter["symbol"] for adapter in table["adapters"]}
        routes = self._approved_routes()

        self.assertEqual(
            routes,
            [
                {
                    "guest": "_getpid",
                    "host": "getpid",
                    "module": "ipasimdarwinhost.dll",
                    "adapter": "_getpid",
                    "owner": "DarwinHostBridge.getpid",
                    "profile": "GeneratedAdapterState",
                }
            ],
            "semantic approval changed without an explicit route-table update",
        )
        for route in routes:
            self.assertIn(
                route["adapter"],
                generated_symbols,
                "approved semantic route is missing generator-owned ABI metadata",
            )

    def test_router_selection_logic_is_table_driven(self):
        router = ROUTER.read_text(encoding="utf-8")
        self.assertIn("ApprovedSemanticImportRoutes", router)
        self.assertIn("findApprovedRoute(hostLookupName, modulePath)", router)
        self.assertIn("registry.describeExecution", router)
        self.assertIn("registry.execute", router)
        self.assertNotIn("NoArgumentsSInt32ToX0", router)
        self.assertNotIn("GuestGetpid", router)
        self.assertNotIn("HostGetpid", router)
        self.assertNotIn("hostLookupName != HostGetpid", router)

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
            "getSelectedGeneratedSemanticImportRequirements",
            translator,
            "SysTranslator no longer derives its live state from the generated adapter",
        )
        self.assertIn(
            "executeSelectedGeneratedSemanticImport",
            translator,
            "SysTranslator no longer executes the loader-selected generated route",
        )
        self.assertNotIn(
            "uint64_t X0 = Emu.readReg(UC_ARM64_REG_X0);\n\n    if constexpr (PrintEmuInfo)",
            translator,
            "generated production routing regressed to the one-register _getpid profile",
        )


if __name__ == "__main__":
    unittest.main()
