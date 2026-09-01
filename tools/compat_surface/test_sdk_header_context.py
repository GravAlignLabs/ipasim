import json
import shutil
import tempfile
import unittest
from pathlib import Path

import header_surface
import sdk_header_surface


class SdkHeaderContextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    @staticmethod
    def write(root: Path, relative: str, text: str) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    @staticmethod
    def compact(value):
        return json.dumps(value, separators=(",", ":"), sort_keys=False)

    def test_framework_leaf_is_parsed_after_its_umbrella_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            headers = "System/Library/Frameworks/Demo.framework/Headers"
            self.write(
                root,
                f"{headers}/DemoBase.h",
                "@interface DemoBase\n@end\n"
                "extern int demo_base_value(void);\n",
            )
            leaf = self.write(
                root,
                f"{headers}/DemoLegacy.h",
                "@interface DemoBase (Legacy)\n@end\n"
                "extern int demo_legacy_value(int value);\n"
                "extern long demo_legacy_other(long value);\n",
            )
            self.write(
                root,
                f"{headers}/Demo.h",
                "#import <Demo/DemoBase.h>\n"
                "#import <Demo/DemoLegacy.h>\n",
            )
            display = f"{headers}/DemoLegacy.h"

            with self.assertRaises(header_surface.HeaderParseError):
                header_surface.analyze_header(
                    leaf,
                    display,
                    sdk_root=root,
                )

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_demo_legacy_other", "_demo_legacy_value"],
            )
            self.assertNotIn("_demo_base_value", json.dumps(manifest))
            self.assertNotIn(str(root), json.dumps(manifest))

    def test_framework_wrapper_preserves_recursive_target_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            headers = "System/Library/Frameworks/Demo.framework/Headers"
            leaf = self.write(
                root,
                f"{headers}/Target.h",
                "typedef int DemoType;\n"
                "extern DemoType target_api(DemoType value);\n",
            )
            self.write(
                root,
                f"{headers}/UsesTarget.h",
                "#import <Demo/Target.h>\n"
                "extern DemoType uses_target(DemoType value);\n",
            )
            self.write(
                root,
                f"{headers}/Demo.h",
                "#import <Demo/UsesTarget.h>\n"
                "#import <Demo/Target.h>\n",
            )
            display = f"{headers}/Target.h"

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_target_api"],
            )
            self.assertNotIn("_uses_target", json.dumps(manifest))

    def test_developer_framework_search_path_is_reconstructed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            headers = "Developer/Library/Frameworks/DevKit.framework/Headers"
            leaf = self.write(
                root,
                f"{headers}/DevLeaf.h",
                "extern long devkit_value(long value);\n",
            )
            self.write(
                root,
                f"{headers}/DevKit.h",
                "#import <DevKit/DevLeaf.h>\n",
            )
            display = f"{headers}/DevLeaf.h"
            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_devkit_value"],
            )

    def test_nested_framework_search_path_has_nearest_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            headers = (
                "System/Library/Frameworks/Outer.framework/Frameworks/"
                "Inner.framework/Headers"
            )
            leaf = self.write(
                root,
                f"{headers}/InnerLeaf.h",
                "extern double inner_value(double value);\n",
            )
            self.write(
                root,
                f"{headers}/Inner.h",
                "#import <Inner/InnerLeaf.h>\n",
            )
            display = f"{headers}/InnerLeaf.h"
            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_inner_value"],
            )

    def test_modular_usr_include_leaf_uses_declared_umbrella_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            package = "usr/include/PacketKit"
            leaf = self.write(
                root,
                f"{package}/PacketLeaf.h",
                "#pragma once\n"
                "#ifndef PACKETKIT_CONTEXT\n"
                "#error Include PacketKit.h instead of this file\n"
                "#endif\n"
                "extern int packet_leaf_value(int value);\n",
            )
            self.write(
                root,
                f"{package}/PacketKit.h",
                "#pragma once\n"
                "#define PACKETKIT_CONTEXT 1\n"
                "#include \"PacketLeaf.h\"\n"
                "extern int packet_umbrella_value(void);\n",
            )
            self.write(
                root,
                f"{package}/module.modulemap",
                'module PacketKit [system] {\n'
                '  umbrella header "PacketKit.h"\n'
                '  export *\n'
                '}\n',
            )
            display = f"{package}/PacketLeaf.h"

            with self.assertRaises(header_surface.HeaderParseError):
                header_surface.analyze_header(
                    leaf,
                    display,
                    sdk_root=root,
                )

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_packet_leaf_value"],
            )
            self.assertNotIn("_packet_umbrella_value", json.dumps(manifest))

    def test_libcxx_leaf_uses_sdk_v1_root_and_objective_cxx_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            libcxx = "usr/include/c++/v1"
            self.write(
                root,
                f"{libcxx}/__config",
                "#pragma once\n#define LIBCPP_CONTEXT 1\n",
            )
            leaf = self.write(
                root,
                f"{libcxx}/__algorithm/probe.h",
                "#pragma once\n"
                "#include <__config>\n"
                "#ifndef LIBCPP_CONTEXT\n#error missing sdk libcxx root\n#endif\n"
                "namespace demo { template <class T> T ignored_template(T value); }\n"
                "extern \"C\" int libcxx_probe(int value);\n",
            )
            display = f"{libcxx}/__algorithm/probe.h"

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_libcxx_probe"],
            )
            self.assertGreaterEqual(
                manifest["summary"]["skipped_cxx_declaration_count"],
                1,
            )

    def test_explicit_public_include_recommendation_supplies_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/os/_private_probe.h",
                "#pragma once\n"
                "#ifndef PUBLIC_PROBE_CONTEXT\n"
                '#error "Do not include this header directly, please include <os/public_probe.h> instead"\n'
                "#endif\n"
                "extern int private_probe_value(int value);\n",
            )
            self.write(
                root,
                "usr/include/os/public_probe.h",
                "#pragma once\n#define PUBLIC_PROBE_CONTEXT 1\n",
            )
            display = "usr/include/os/_private_probe.h"

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_private_probe_value"],
            )

    def test_usr_include_leaf_recovers_through_textual_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            self.write(
                root,
                "usr/include/rpc/types.h",
                "#pragma once\ntypedef int rpc_bool_t;\n",
            )
            leaf = self.write(
                root,
                "usr/include/rpc/auth_probe.h",
                "#pragma once\n"
                "extern rpc_bool_t rpc_probe(rpc_bool_t value);\n",
            )
            self.write(
                root,
                "usr/include/rpc/rpc.h",
                "#pragma once\n"
                "#include <rpc/types.h>\n"
                "#include <rpc/auth_probe.h>\n",
            )
            display = "usr/include/rpc/auth_probe.h"

            with self.assertRaises(header_surface.HeaderParseError):
                header_surface.analyze_header(leaf, display, sdk_root=root)

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_rpc_probe"],
            )

    def test_libcxx_conditional_owner_records_target_inactive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            libcxx = "usr/include/c++/v1"
            self.write(root, f"{libcxx}/__config", "#pragma once\n")
            leaf = self.write(
                root,
                f"{libcxx}/__support/vendor/only.h",
                "#pragma once\nunknown_vendor_type impossible_here;\n",
            )
            self.write(
                root,
                f"{libcxx}/__owner",
                "#pragma once\n"
                "#include <__config>\n"
                "#ifdef __VENDOR_ONLY__\n"
                "#include <__support/vendor/only.h>\n"
                "#endif\n",
            )
            display = f"{libcxx}/__support/vendor/only.h"

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            inactive = manifest["target_inactive_headers"]
            self.assertEqual(inactive[0]["header"], display)
            self.assertEqual(inactive[0]["context_header"], f"{libcxx}/__owner")
            self.assertIn("does not activate", inactive[0]["reason"])

    def test_unreferenced_libcxx_support_leaf_is_explicitly_inactive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            libcxx = "usr/include/c++/v1"
            self.write(root, f"{libcxx}/__config", "#pragma once\n")
            leaf = self.write(
                root,
                f"{libcxx}/__support/orphan/dead.h",
                "#pragma once\nunknown_orphan_type impossible_here;\n",
            )
            display = f"{libcxx}/__support/orphan/dead.h"

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(manifest["signatures"], [])
            self.assertEqual(manifest["summary"]["target_inactive_header_count"], 1)
            self.assertNotIn(
                "context_header",
                manifest["target_inactive_headers"][0],
            )
            self.assertIn(
                "not reachable",
                manifest["target_inactive_headers"][0]["reason"],
            )

    def test_target_unavailable_declaration_is_recorded_not_silently_dropped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            headers = "System/Library/Frameworks/Demo.framework/Headers"
            leaf = self.write(
                root,
                f"{headers}/DemoAPI.h",
                "__attribute__((availability(ios,unavailable))) "
                "extern int desktop_only(int value);\n"
                "extern int mobile_ok(int value);\n",
            )
            self.write(
                root,
                f"{headers}/Demo.h",
                "#import <Demo/DemoAPI.h>\n",
            )
            display = f"{headers}/DemoAPI.h"
            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )

            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_mobile_ok"],
            )
            self.assertEqual(
                manifest["summary"]["target_unavailable_declaration_count"],
                1,
            )
            unavailable = manifest["target_unavailable"]
            self.assertEqual(len(unavailable), 1)
            self.assertEqual(unavailable[0]["symbol"], "_desktop_only")
            self.assertEqual(unavailable[0]["name"], "desktop_only")
            self.assertEqual(unavailable[0]["source"]["header"], display)
            self.assertIn("not available on iOS", unavailable[0]["reason"])

    def test_ordinary_target_unavailable_declaration_uses_same_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            leaf = self.write(
                root,
                "usr/include/legacy_api.h",
                "__attribute__((availability(ios,unavailable))) "
                "extern int legacy_only(int value);\n"
                "extern int current_value(int value);\n",
            )
            display = "usr/include/legacy_api.h"

            with self.assertRaises(header_surface.HeaderParseError):
                header_surface.analyze_header(leaf, display, sdk_root=root)

            manifest = sdk_header_surface.build_parallel_manifest(
                [(leaf, display)],
                jobs=1,
                sdk_root=root,
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_current_value"],
            )
            self.assertEqual(
                [item["symbol"] for item in manifest["target_unavailable"]],
                ["_legacy_only"],
            )

    def test_unavailable_provenance_survives_exhaustive_shard_merge(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            headers = "System/Library/Frameworks/Demo.framework/Headers"
            paths = [
                self.write(
                    root,
                    f"{headers}/A.h",
                    "__attribute__((availability(ios,unavailable))) "
                    "extern int old_api(void);\n",
                ),
                self.write(
                    root,
                    f"{headers}/B.h",
                    "extern int current_api(void);\n",
                ),
                self.write(
                    root,
                    f"{headers}/Demo.h",
                    "#import <Demo/A.h>\n#import <Demo/B.h>\n",
                ),
            ]
            inputs = sorted(
                [(path, path.relative_to(root).as_posix()) for path in paths],
                key=lambda item: item[1],
            )
            baseline = sdk_header_surface.build_parallel_manifest(
                inputs,
                jobs=2,
                sdk_root=root,
            )

            shards = []
            for shard_index in range(2):
                selected = sdk_header_surface.select_shard(
                    inputs,
                    shard_count=2,
                    shard_index=shard_index,
                )
                manifest = sdk_header_surface.build_parallel_manifest(
                    selected,
                    jobs=1,
                    sdk_root=root,
                )
                shards.append(
                    sdk_header_surface.attach_shard_coverage(
                        manifest,
                        all_inputs=inputs,
                        shard_count=2,
                        shard_index=shard_index,
                    )
                )

            merged = sdk_header_surface.merge_shard_manifests(
                list(reversed(shards)),
                expected_headers=[display for _, display in inputs],
            )
            self.assertEqual(self.compact(merged), self.compact(baseline))
            self.assertEqual(
                merged["summary"]["target_unavailable_declaration_count"],
                1,
            )

    def test_target_inactive_provenance_survives_exhaustive_shard_merge(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Synthetic.sdk"
            libcxx = "usr/include/c++/v1"
            self.write(root, f"{libcxx}/__config", "#pragma once\n")
            inactive = self.write(
                root,
                f"{libcxx}/__support/vendor/inactive.h",
                "#pragma once\nunknown_vendor_type impossible_here;\n",
            )
            owner = self.write(
                root,
                f"{libcxx}/__owner",
                "#pragma once\n"
                "#ifdef __VENDOR_ONLY__\n"
                "#include <__support/vendor/inactive.h>\n"
                "#endif\n",
            )
            active = self.write(
                root,
                "usr/include/active.h",
                "extern int active_value(void);\n",
            )
            inputs = sorted(
                [
                    (inactive, inactive.relative_to(root).as_posix()),
                    (owner, owner.relative_to(root).as_posix()),
                    (active, active.relative_to(root).as_posix()),
                ],
                key=lambda item: item[1],
            )
            baseline = sdk_header_surface.build_parallel_manifest(
                inputs,
                jobs=2,
                sdk_root=root,
            )

            shards = []
            for shard_index in range(2):
                selected = sdk_header_surface.select_shard(
                    inputs,
                    shard_count=2,
                    shard_index=shard_index,
                )
                manifest = sdk_header_surface.build_parallel_manifest(
                    selected,
                    jobs=1,
                    sdk_root=root,
                )
                shards.append(
                    sdk_header_surface.attach_shard_coverage(
                        manifest,
                        all_inputs=inputs,
                        shard_count=2,
                        shard_index=shard_index,
                    )
                )

            merged = sdk_header_surface.merge_shard_manifests(
                list(reversed(shards)),
                expected_headers=[display for _, display in inputs],
            )
            self.assertEqual(self.compact(merged), self.compact(baseline))
            self.assertEqual(merged["summary"]["target_inactive_header_count"], 1)
            self.assertEqual(
                merged["target_inactive_headers"][0]["header"],
                f"{libcxx}/__support/vendor/inactive.h",
            )


if __name__ == "__main__":
    unittest.main()
