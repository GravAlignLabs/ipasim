import copy
import json
import pathlib
import tempfile
import unittest

import generate_semantic_routes as routes


ROOT = pathlib.Path(__file__).resolve().parents[2]
PROVIDERS = pathlib.Path(__file__).with_name("semantic_providers.json")
APPROVED_ROUTES = ROOT / "src" / "IpaSimulator" / "ApprovedSemanticImportRoutes.inc"


class SemanticRouteGeneratorTests(unittest.TestCase):
    def load_production_manifest(self):
        return json.loads(PROVIDERS.read_text(encoding="utf-8"))

    def test_production_route_table_has_no_drift(self):
        manifest = self.load_production_manifest()
        rendered = routes.render_cpp(manifest)
        self.assertEqual(
            APPROVED_ROUTES.read_text(encoding="utf-8"),
            rendered,
            "production semantic route table drifted from semantic_providers.json",
        )

    def test_only_approved_records_are_emitted(self):
        manifest = self.load_production_manifest()
        manifest["providers"].append(
            {
                "guest_symbol": "_future_symbol",
                "status": "complex",
                "evidence": "Requires a stateful Darwin subsystem that is not approved yet.",
            }
        )
        rendered = routes.render_cpp(manifest)
        self.assertIn('"_getpid"', rendered)
        self.assertNotIn("_future_symbol", rendered)

    def test_non_approved_record_cannot_carry_stale_route_fields(self):
        manifest = self.load_production_manifest()
        stale = copy.deepcopy(manifest["providers"][0])
        stale["guest_symbol"] = "_candidate"
        stale["status"] = "candidate"
        manifest["providers"].append(stale)
        with self.assertRaisesRegex(routes.SemanticRouteError, "carries route fields"):
            routes.render_cpp(manifest)

    def test_duplicate_guest_symbol_fails_closed(self):
        manifest = self.load_production_manifest()
        manifest["providers"].append(copy.deepcopy(manifest["providers"][0]))
        with self.assertRaisesRegex(routes.SemanticRouteError, "repeats guest symbol"):
            routes.render_cpp(manifest)

    def test_duplicate_approved_provider_export_fails_closed(self):
        manifest = self.load_production_manifest()
        duplicate = copy.deepcopy(manifest["providers"][0])
        duplicate["guest_symbol"] = "_other_getpid"
        duplicate["adapter_symbol"] = "_other_getpid"
        manifest["providers"].append(duplicate)
        with self.assertRaisesRegex(routes.SemanticRouteError, "repeats approved provider export"):
            routes.render_cpp(manifest)

    def test_unknown_live_profile_fails_closed(self):
        manifest = self.load_production_manifest()
        manifest["providers"][0]["live_profile"] = "InventedProfile"
        with self.assertRaisesRegex(routes.SemanticRouteError, "unsupported live profile"):
            routes.render_cpp(manifest)

    def test_check_mode_detects_drift(self):
        manifest = self.load_production_manifest()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            providers = root / "providers.json"
            output = root / "routes.inc"
            providers.write_text(json.dumps(manifest), encoding="utf-8")
            output.write_text("stale\n", encoding="utf-8")
            self.assertEqual(
                routes.main(
                    [
                        "--providers",
                        str(providers),
                        "--output",
                        str(output),
                        "--check",
                    ]
                ),
                1,
            )
            output.write_text(routes.render_cpp(manifest), encoding="utf-8")
            self.assertEqual(
                routes.main(
                    [
                        "--providers",
                        str(providers),
                        "--output",
                        str(output),
                        "--check",
                    ]
                ),
                0,
            )


if __name__ == "__main__":
    unittest.main()
