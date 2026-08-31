import json
import shutil
import tempfile
import unittest
from pathlib import Path

import sdk_compatibility
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

    def test_one_command_runs_real_complete_pipeline_and_writes_bundle(self):
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
                    "--semantic-providers",
                    str(SEMANTIC_PROVIDERS),
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
