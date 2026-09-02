import copy
import unittest

import semantic_migration


class SemanticMigrationTests(unittest.TestCase):
    def host_surface(self):
        return {
            "schema_version": 1,
            "kind": "pe-def-export-surface",
            "library": "IpaSimDarwinHost",
            "library_filename": "IpaSimDarwinHost.dll",
            "exports": [
                {"name": "getpid", "target": "darwin_getpid", "kind": "function"},
                {"name": "getuid", "target": "darwin_getuid", "kind": "function"},
                {"name": "complex_api", "target": "darwin_complex_api", "kind": "function"},
                {"name": "without_adapter", "target": "darwin_without_adapter", "kind": "function"},
                {"name": "global_value", "target": "global_value", "kind": "data"},
            ],
        }

    def adapters(self):
        return {
            "schema_version": 1,
            "kind": "runtime-adapter-table",
            "adapters": [
                {"symbol": "_getpid"},
                {"symbol": "_getuid"},
                {"symbol": "_complex_api"},
                {"symbol": "_global_value"},
            ],
        }

    def semantics(self):
        return {
            "schema_version": 1,
            "kind": "semantic-provider-inventory",
            "providers": [
                {
                    "guest_symbol": "_getpid",
                    "status": "approved",
                    "host_export": "getpid",
                    "provider_module": "ipasimdarwinhost.dll",
                    "adapter_symbol": "_getpid",
                    "semantic_owner": "DarwinHostBridge.getpid",
                    "live_profile": "NoArgumentsSInt32ToX0",
                    "evidence": "approved fixture",
                },
                {
                    "guest_symbol": "_complex_api",
                    "status": "complex",
                    "evidence": "requires stateful guest behavior",
                },
            ],
        }

    def test_only_unapproved_callable_export_with_adapter_becomes_candidate(self):
        plan = semantic_migration.build_migration_plan(
            self.host_surface(),
            self.adapters(),
            self.semantics(),
        )
        by_symbol = {item["guest_symbol"]: item for item in plan["exports"]}

        self.assertEqual(plan["candidate_symbols"], ["_getuid"])
        self.assertEqual(by_symbol["_getpid"]["migration_status"], "already-approved")
        self.assertEqual(by_symbol["_getuid"]["migration_status"], "migration-candidate")
        self.assertEqual(by_symbol["_complex_api"]["migration_status"], "semantic-complex")
        self.assertEqual(by_symbol["_without_adapter"]["migration_status"], "no-generated-adapter")
        self.assertEqual(by_symbol["_global_value"]["migration_status"], "data-export")
        self.assertEqual(plan["summary"]["migration_candidate_count"], 1)

    def test_explicit_candidate_status_remains_unapproved_but_queued(self):
        semantics = self.semantics()
        semantics["providers"].append(
            {
                "guest_symbol": "_getuid",
                "status": "candidate",
                "evidence": "implementation exists but still needs semantic review",
            }
        )
        plan = semantic_migration.build_migration_plan(
            self.host_surface(),
            self.adapters(),
            semantics,
        )
        row = next(item for item in plan["exports"] if item["guest_symbol"] == "_getuid")
        self.assertEqual(row["semantic_status"], "candidate")
        self.assertEqual(row["migration_status"], "migration-candidate")

    def test_approved_route_must_match_exact_def_identity(self):
        semantics = self.semantics()
        broken = copy.deepcopy(semantics)
        broken["providers"][0]["host_export"] = "different_export"
        with self.assertRaisesRegex(
            semantic_migration.SemanticMigrationError,
            "host export differs",
        ):
            semantic_migration.build_migration_plan(
                self.host_surface(),
                self.adapters(),
                broken,
            )

    def test_data_export_never_becomes_candidate_even_with_adapter(self):
        plan = semantic_migration.build_migration_plan(
            self.host_surface(),
            self.adapters(),
            self.semantics(),
        )
        row = next(item for item in plan["exports"] if item["guest_symbol"] == "_global_value")
        self.assertTrue(row["generated_adapter_available"])
        self.assertEqual(row["migration_status"], "data-export")
        self.assertNotIn("_global_value", plan["candidate_symbols"])


if __name__ == "__main__":
    unittest.main()
