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
                "@interface DemoBase\n@end\n",
            )
            leaf = self.write(
                root,
                f"{headers}/DemoLegacy.h",
                "@interface DemoBase (Legacy)\n@end\n"
                "extern int demo_legacy_value(int value);\n",
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
                ["_demo_legacy_value"],
            )
            self.assertNotIn(str(root), json.dumps(manifest))

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


if __name__ == "__main__":
    unittest.main()
