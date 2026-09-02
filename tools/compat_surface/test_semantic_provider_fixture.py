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

PROCESS_IDENTITY = {
    "_getegid": ("getegid", "DarwinCredentialAdapter.getegid"),
    "_geteuid": ("geteuid", "DarwinCredentialAdapter.geteuid"),
    "_getgid": ("getgid", "DarwinCredentialAdapter.getgid"),
    "_getpid": ("getpid", "DarwinHostBridge.getpid"),
    "_getuid": ("getuid", "DarwinCredentialAdapter.getuid"),
}
SCALAR_DESCRIPTOR = {
    "_close": ("close", "DarwinHostBridge.close"),
    "_lseek": ("lseek", "DarwinHostBridge.lseek"),
}
POINTER_DESCRIPTOR = {
    "_pread": ("pread", "DarwinKernelBridge.pread"),
    "_pwrite": ("pwrite", "DarwinKernelBridge.pwrite"),
    "_write": ("write", "DarwinKernelBridge.write"),
}
APPROVED_PRODUCTION = {
    **PROCESS_IDENTITY,
    **SCALAR_DESCRIPTOR,
    **POINTER_DESCRIPTOR,
}


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

    def _production_adapter_symbols(self):
        source = PRODUCTION_ADAPTERS.read_text(encoding="utf-8")
        return re.findall(r'AdapterRecord\{\s*"([^"]+)"', source)

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

    def test_production_generated_adapter_set_matches_approved_sdk_subset(self):
        self.assertEqual(
            self._production_adapter_symbols(),
            sorted(APPROVED_PRODUCTION),
            "production semantic adapter set is not the explicitly approved SDK-backed subset",
        )

    def test_process_identity_adapters_match_sdk_abi(self):
        source = PRODUCTION_ADAPTERS.read_text(encoding="utf-8")
        for guest in PROCESS_IDENTITY:
            marker = f'AdapterRecord{{\n            "{guest}",'
            self.assertIn(marker, source)
        self.assertNotIn(
            "ValueTypeKind::SInt32",
            source,
            "production generated routes drifted from the full-SDK unsigned carrier evidence",
        )

    def test_scalar_descriptor_adapters_match_full_sdk_abi(self):
        source = PRODUCTION_ADAPTERS.read_text(encoding="utf-8")
        close_pattern = re.compile(
            r'AdapterRecord\{\s*"_close"\s*,\s*false\s*,\s*\{\s*'
            r'ArgumentSpec\{\s*0\s*,\s*0\s*,\s*true\s*,\s*'
            r'TypeSpec::builtin\(ValueTypeKind::UInt32\)\s*,\s*'
            r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{0\}, 4\)\s*,\s*false\}\s*,\s*'
            r'\}\s*,\s*ResultSpec\{\s*'
            r'TypeSpec::builtin\(ValueTypeKind::UInt32\)\s*,\s*'
            r'CommitSpec::toRegisters\(GuestBank::Gpr, \{0\}, 4\)\}\}',
            re.DOTALL,
        )
        lseek_pattern = re.compile(
            r'AdapterRecord\{\s*"_lseek"\s*,\s*false\s*,\s*\{\s*'
            r'ArgumentSpec\{\s*0\s*,\s*0\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt32\).*?'
            r'ArgumentSpec\{\s*1\s*,\s*1\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt64\).*?'
            r'ArgumentSpec\{\s*2\s*,\s*2\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt32\).*?'
            r'ResultSpec\{\s*TypeSpec::builtin\(ValueTypeKind::UInt64\)\s*,\s*'
            r'CommitSpec::toRegisters\(GuestBank::Gpr, \{0\}, 8\)\}\}',
            re.DOTALL,
        )
        self.assertRegex(source, close_pattern)
        self.assertRegex(source, lseek_pattern)

    def test_pointer_descriptor_adapters_match_full_sdk_abi(self):
        source = PRODUCTION_ADAPTERS.read_text(encoding="utf-8")
        write_pattern = re.compile(
            r'AdapterRecord\{\s*"_write"\s*,\s*true\s*,\s*\{\s*'
            r'ArgumentSpec\{\s*0\s*,\s*0\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt32\).*?'
            r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{0\}, 4\).*?'
            r'ArgumentSpec\{\s*1\s*,\s*1\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::Pointer\).*?'
            r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{1\}, 8\).*?'
            r'ArgumentSpec\{\s*2\s*,\s*2\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt64\).*?'
            r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{2\}, 8\).*?'
            r'ResultSpec\{\s*TypeSpec::builtin\(ValueTypeKind::UInt64\)\s*,\s*'
            r'CommitSpec::toRegisters\(GuestBank::Gpr, \{0\}, 8\)\}\}',
            re.DOTALL,
        )
        self.assertRegex(source, write_pattern)

        for guest in ("_pread", "_pwrite"):
            positional_pattern = re.compile(
                r'AdapterRecord\{\s*"' + re.escape(guest) + r'"\s*,\s*true\s*,\s*\{\s*'
                r'ArgumentSpec\{\s*0\s*,\s*0\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt32\).*?'
                r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{0\}, 4\).*?'
                r'ArgumentSpec\{\s*1\s*,\s*1\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::Pointer\).*?'
                r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{1\}, 8\).*?'
                r'ArgumentSpec\{\s*2\s*,\s*2\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt64\).*?'
                r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{2\}, 8\).*?'
                r'ArgumentSpec\{\s*3\s*,\s*3\s*,\s*true\s*,\s*TypeSpec::builtin\(ValueTypeKind::UInt64\).*?'
                r'CaptureSpec::fromRegisters\(GuestBank::Gpr, \{3\}, 8\).*?'
                r'ResultSpec\{\s*TypeSpec::builtin\(ValueTypeKind::UInt64\)\s*,\s*'
                r'CommitSpec::toRegisters\(GuestBank::Gpr, \{0\}, 8\)\}\}',
                re.DOTALL,
            )
            self.assertRegex(source, positional_pattern)

        self.assertNotIn(
            'AdapterRecord{\n            "_read",',
            source,
            "read must remain unapproved until socket receive semantics are complete",
        )

    def test_approved_route_table_is_explicit_and_backed_by_generated_abi(self):
        generated_symbols = set(self._production_adapter_symbols())
        routes = self._approved_routes()

        expected = []
        for guest in sorted(APPROVED_PRODUCTION):
            host, owner = APPROVED_PRODUCTION[guest]
            expected.append(
                {
                    "guest": guest,
                    "host": host,
                    "module": "ipasimdarwinhost.dll",
                    "adapter": guest,
                    "owner": owner,
                    "profile": "GeneratedAdapterState",
                }
            )
        self.assertEqual(
            routes,
            expected,
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

    def test_generated_routes_are_not_duplicated_in_handwritten_darwin_abi_table(self):
        translator = SYS_TRANSLATOR.read_text(encoding="utf-8")
        for guest, (host, _) in APPROVED_PRODUCTION.items():
            handwritten = re.compile(r'\{\s*"' + re.escape(host) + r'"\s*,\s*\d+\s*,')
            self.assertIsNone(
                handwritten.search(translator),
                f"{guest} is generated and must not also exist in the handwritten Darwin ABI table",
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
