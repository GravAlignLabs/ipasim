import tempfile
import unittest
from pathlib import Path
from unittest import mock

import bulk_compatibility


class BulkCompatibilityTests(unittest.TestCase):
    def test_pipeline_runs_complete_sdk_surface_without_macho_manifest(self):
        catalog = {"kind": "typed-sdk-catalog"}
        inventory = {"kind": "typed-compatibility-inventory"}
        guest = {"kind": "aapcs64-abi-surface"}
        host = {"kind": "win64-carrier-abi-surface"}
        ffi_plan = {"kind": "libffi-bridge-adapter-plan"}
        adapters = {
            "schema_version": 1,
            "kind": "runtime-adapter-table",
            "adapters": [{"symbol": "_getpid"}],
        }
        plan = {"kind": "sdk-compatibility-plan"}
        semantics = {
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
                    "evidence": "test",
                }
            ],
        }

        with (
            mock.patch.object(
                bulk_compatibility.sdk_catalog,
                "build_sdk_catalog",
                return_value=catalog,
            ) as build_catalog,
            mock.patch.object(
                bulk_compatibility.sdk_catalog,
                "build_abi_inventory",
                return_value=inventory,
            ) as build_inventory,
            mock.patch.object(
                bulk_compatibility.sdk_abi_context,
                "build_aapcs64_manifest",
                return_value=guest,
            ) as build_guest,
            mock.patch.object(
                bulk_compatibility.compiler_batching,
                "build_win64_manifest",
                return_value=host,
            ) as build_host,
            mock.patch.object(
                bulk_compatibility.bridge_plan,
                "build_bridge_plan",
                return_value=ffi_plan,
            ) as build_bridge,
            mock.patch.object(
                bulk_compatibility.runtime_adapter_table,
                "build_runtime_adapter_table",
                return_value=adapters,
            ) as build_adapters,
            mock.patch.object(
                bulk_compatibility.compat_planner,
                "build_plan",
                return_value=plan,
            ) as build_plan,
            mock.patch.object(
                bulk_compatibility.generate_semantic_routes,
                "render_cpp",
                return_value="ROUTES\n",
            ) as render_routes,
            mock.patch.object(
                bulk_compatibility.runtime_adapter_table,
                "render_cpp_table",
                return_value="ADAPTERS\n",
            ) as render_adapters,
        ):
            outputs = bulk_compatibility.run_pipeline(
                tapi_manifest={"tapi": True},
                header_manifest={"headers": True},
                semantic_manifest=semantics,
                header_root=Path("headers"),
                sdk_root=Path("sdk"),
                clang="clang",
                host_target="x86_64-pc-windows-msvc",
                clang_args=("-DTEST",),
                timeout_seconds=30,
                compiler_batch_size=17,
            )

        build_catalog.assert_called_once_with({"tapi": True}, {"headers": True})
        build_inventory.assert_called_once_with(catalog)
        build_guest.assert_called_once_with(
            inventory,
            header_root=Path("headers"),
            clang="clang",
            sdk_root=Path("sdk"),
            extra_args=("-DTEST",),
            timeout_seconds=30,
            batch_size=17,
        )
        build_host.assert_called_once_with(
            guest,
            clang="clang",
            host_target="x86_64-pc-windows-msvc",
            extra_args=("-DTEST",),
            timeout_seconds=30,
            batch_size=17,
        )
        build_bridge.assert_called_once_with(host)
        build_adapters.assert_called_once_with(ffi_plan)
        build_plan.assert_called_once_with(catalog, semantics, abi_manifest=guest)
        render_routes.assert_called_once_with(semantics)
        render_adapters.assert_called_once_with(
            adapters, "makeGeneratedSdkWideAdapterTable"
        )
        self.assertEqual(outputs["routes_cpp"], "ROUTES\n")
        self.assertEqual(outputs["adapters_cpp"], "ADAPTERS\n")

    def test_approved_route_without_generated_adapter_fails_closed(self):
        semantics = {
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
                    "evidence": "test",
                }
            ],
        }
        adapters = {
            "schema_version": 1,
            "kind": "runtime-adapter-table",
            "adapters": [],
        }
        with self.assertRaisesRegex(
            bulk_compatibility.BulkCompatibilityError,
            "has no SDK-wide generated runtime adapter",
        ):
            bulk_compatibility._validate_approved_routes_have_generated_abi(
                semantics, adapters
            )

    def test_write_outputs_materializes_complete_planner_bundle(self):
        outputs = {
            "sdk_catalog": {"kind": "typed-sdk-catalog"},
            "abi_inventory": {"kind": "typed-compatibility-inventory"},
            "guest_abi": {"kind": "aapcs64-abi-surface"},
            "host_abi": {"kind": "win64-carrier-abi-surface"},
            "bridge_plan": {"kind": "libffi-bridge-adapter-plan"},
            "runtime_adapters": {"kind": "runtime-adapter-table"},
            "compatibility_plan": {"kind": "sdk-compatibility-plan"},
            "routes_cpp": "ROUTES\n",
            "adapters_cpp": "ADAPTERS\n",
        }
        expected = {
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
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bulk_compatibility.write_outputs(root, outputs)
            self.assertEqual({path.name for path in root.iterdir()}, expected)
            self.assertEqual(
                (root / "GeneratedSdkAdapters.inc").read_text(), "ADAPTERS\n"
            )
            self.assertEqual(
                (root / "ApprovedSemanticImportRoutes.inc").read_text(), "ROUTES\n"
            )


if __name__ == "__main__":
    unittest.main()
