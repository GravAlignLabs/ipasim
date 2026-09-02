import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sdk_compatibility
import sdk_header_surface
import tbd_surface


ROOT = Path(__file__).resolve().parents[2]
SEMANTIC_PROVIDERS = Path(__file__).with_name("semantic_providers.json")


class SdkCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")
        if tbd_surface.yaml is None:
            raise unittest.SkipTest("PyYAML is required")

    @staticmethod
    def write_sdk(root: Path, *, symbol: str = "_getpid", c_name: str = "getpid") -> None:
        library = root / "usr" / "lib"
        include = root / "usr" / "include"
        library.mkdir(parents=True)
        include.mkdir(parents=True)
        (library / "libSystem.B.tbd").write_text(
            "--- !tapi-tbd-v4\n"
            "tbd-version: 4\n"
            "targets: [ arm64-ios ]\n"
            "install-name: '/usr/lib/libSystem.B.dylib'\n"
            "current-version: 1319.100.3\n"
            "compatibility-version: 1.0.0\n"
            "exports:\n"
            "  - targets: [ arm64-ios ]\n"
            f"    symbols: [ {symbol} ]\n"
            "...\n",
            encoding="utf-8",
        )
        (include / "unistd.h").write_text(
            f"int {c_name}(void);\n",
            encoding="utf-8",
        )

    @staticmethod
    def write_single_header_shard(sdk: Path, destination: Path) -> None:
        inputs = sdk_compatibility._collect_headers(sdk)
        manifest = sdk_header_surface.build_parallel_manifest(
            inputs,
            jobs=1,
            sdk_root=sdk,
        )
        manifest = sdk_header_surface.attach_shard_coverage(
            manifest,
            all_inputs=inputs,
            shard_count=1,
            shard_index=0,
        )
        destination.write_text(
            json.dumps(manifest, indent=2) + "\n",
            encoding="utf-8",
        )

    def test_one_command_runs_real_complete_pipeline_and_writes_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "SyntheticPublic.sdk"
            self.write_sdk(sdk)
            host_def = root / "SyntheticHost.def"
            host_def.write_text(
                "LIBRARY IpaSimDarwinHost\nEXPORTS\n    getpid=darwin_getpid\n",
                encoding="utf-8",
            )
            output = root / "bundle"

            code = sdk_compatibility.main(
                [
                    "--sdk-root",
                    str(sdk),
                    "--output-dir",
                    str(output),
                    "--semantic-providers",
                    str(SEMANTIC_PROVIDERS),
                    "--host-export-def",
                    str(host_def),
                    "--header-jobs",
                    "2",
                    "--compiler-batch-size",
                    "1",
                ]
            )
            self.assertEqual(code, 0)

            expected = {
                "tapi-sdk-surface.json",
                "header-signatures.json",
                "sdk-catalog.json",
                "sdk-abi-inventory.json",
                "aapcs64-abi.json",
                "win64-abi.json",
                "bridge-plan.json",
                "runtime-adapters.json",
                "compatibility-plan.json",
                "semantic-migration-plan.json",
                "GeneratedSdkAdapters.inc",
                "ApprovedSemanticImportRoutes.inc",
            }
            self.assertEqual({path.name for path in output.iterdir()}, expected)

            tapi = json.loads((output / "tapi-sdk-surface.json").read_text())
            headers = json.loads((output / "header-signatures.json").read_text())
            guest = json.loads((output / "aapcs64-abi.json").read_text())
            host = json.loads((output / "win64-abi.json").read_text())
            adapters = json.loads((output / "runtime-adapters.json").read_text())
            plan = json.loads((output / "compatibility-plan.json").read_text())
            migration = json.loads((output / "semantic-migration-plan.json").read_text())

            self.assertEqual(tapi["summary"]["unique_symbol_count"], 1)
            self.assertEqual(headers["summary"]["unique_symbol_count"], 1)
            self.assertEqual(headers["signatures"][0]["symbol"], "_getpid")
            self.assertEqual(
                guest["symbols"][0]["bridge_status"],
                "generated-bridge-candidate",
            )
            self.assertEqual(
                host["symbols"][0]["cross_abi_status"],
                "cross-abi-adapter-candidate",
            )
            self.assertEqual(
                [item["symbol"] for item in adapters["adapters"]],
                ["_getpid"],
            )
            self.assertEqual(
                plan["summary"]["route_status_counts"][
                    "approved-mechanical-route-ready"
                ],
                1,
            )
            self.assertEqual(migration["summary"]["already_approved_count"], 1)
            self.assertEqual(migration["summary"]["migration_candidate_count"], 0)
            self.assertEqual(migration["candidate_symbols"], [])
            self.assertIn(
                '"_getpid"',
                (output / "ApprovedSemanticImportRoutes.inc").read_text(),
            )

            private_root = str(root.resolve())
            for path in output.iterdir():
                self.assertNotIn(
                    private_root,
                    path.read_text(encoding="utf-8"),
                    path.name,
                )

    def test_precomputed_header_shard_runs_same_complete_pipeline_without_rescan(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "SyntheticPublic.sdk"
            self.write_sdk(sdk)
            shard = root / "header-shard-00.json"
            self.write_single_header_shard(sdk, shard)
            output = root / "bundle"

            with mock.patch.object(
                sdk_compatibility,
                "_build_header_manifest",
                side_effect=AssertionError("header scan must not run in merge stage"),
            ):
                code = sdk_compatibility.main(
                    [
                        "--sdk-root",
                        str(sdk),
                        "--output-dir",
                        str(output),
                        "--semantic-providers",
                        str(SEMANTIC_PROVIDERS),
                        "--header-manifest",
                        str(shard),
                        "--compiler-batch-size",
                        "1",
                    ]
                )
            self.assertEqual(code, 0)
            headers = json.loads((output / "header-signatures.json").read_text())
            self.assertEqual(headers["summary"]["header_count"], 1)
            self.assertEqual(headers["signatures"][0]["symbol"], "_getpid")
            self.assertNotIn("coverage", headers)

    def test_precomputed_header_shard_with_incomplete_coverage_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "SyntheticPublic.sdk"
            self.write_sdk(sdk)
            shard = root / "header-shard-00.json"
            self.write_single_header_shard(sdk, shard)
            manifest = json.loads(shard.read_text(encoding="utf-8"))
            manifest["coverage"]["headers"] = []
            shard.write_text(json.dumps(manifest), encoding="utf-8")
            output = root / "bundle"

            code = sdk_compatibility.main(
                [
                    "--sdk-root",
                    str(sdk),
                    "--output-dir",
                    str(output),
                    "--semantic-providers",
                    str(SEMANTIC_PROVIDERS),
                    "--header-manifest",
                    str(shard),
                    "--compiler-batch-size",
                    "1",
                ]
            )
            self.assertEqual(code, 1)
            self.assertFalse(output.exists())

    def test_missing_approved_sdk_adapter_aborts_without_output_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "SyntheticPublic.sdk"
            self.write_sdk(sdk, symbol="_other", c_name="other")
            output = root / "bundle"

            code = sdk_compatibility.main(
                [
                    "--sdk-root",
                    str(sdk),
                    "--output-dir",
                    str(output),
                    "--semantic-providers",
                    str(SEMANTIC_PROVIDERS),
                    "--header-jobs",
                    "1",
                    "--compiler-batch-size",
                    "1",
                ]
            )
            self.assertEqual(code, 1)
            self.assertFalse(output.exists())

    def test_relative_header_escape_fails_before_materializing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "SyntheticPublic.sdk"
            self.write_sdk(sdk)
            output = root / "bundle"

            code = sdk_compatibility.main(
                [
                    "--sdk-root",
                    str(sdk),
                    "--output-dir",
                    str(output),
                    "--relative-header",
                    "../outside.h",
                ]
            )
            self.assertEqual(code, 1)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
