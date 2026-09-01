import argparse
import json
import os
import tempfile
import unittest
from pathlib import Path

import header_surface
import sdk_compatibility
import sdk_header_exhaustive as sdk_header_surface


class SdkCompatibilityInventoryTests(unittest.TestCase):
    def _write_sdk_with_header_alias(self, root: Path) -> Path:
        sdk = root / "SyntheticAlias.sdk"
        include = sdk / "usr" / "include"
        include.mkdir(parents=True)
        (include / "z_real.h").write_text("int real_symbol(void);\n", encoding="utf-8")
        (include / "b_other.h").write_text("int other_symbol(void);\n", encoding="utf-8")
        alias = include / "a_alias.h"
        try:
            alias.symlink_to("z_real.h")
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"header symlinks are unavailable: {exc}")
        return sdk

    def test_collect_headers_matches_shard_scanner_for_header_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = self._write_sdk_with_header_alias(Path(directory))
            args = argparse.Namespace(
                sdk_root=str(sdk),
                relative_header=None,
                headers=[],
            )
            shard_inputs, _ = header_surface._resolve_inputs(args)
            merge_inputs = sdk_compatibility._collect_headers(sdk)

            self.assertEqual(
                [(str(path), display) for path, display in merge_inputs],
                [(str(path), display) for path, display in shard_inputs],
            )
            self.assertEqual(len(merge_inputs), 2)
            self.assertEqual(
                [display for _, display in merge_inputs],
                ["usr/include/a_alias.h", "usr/include/b_other.h"],
            )

    def test_precomputed_shard_merge_accepts_canonical_alias_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = self._write_sdk_with_header_alias(root)
            args = argparse.Namespace(
                sdk_root=str(sdk),
                relative_header=None,
                headers=[],
            )
            shard_inputs, _ = header_surface._resolve_inputs(args)
            manifest = header_surface.build_manifest(
                [],
                target=header_surface.DEFAULT_TARGET,
                headers=[display for _, display in shard_inputs],
            )
            manifest = sdk_header_surface.attach_shard_coverage(
                manifest,
                all_inputs=shard_inputs,
                shard_count=1,
                shard_index=0,
            )
            shard = root / "header-shard-0.json"
            shard.write_text(json.dumps(manifest), encoding="utf-8")

            merged = sdk_compatibility._merge_header_shards(
                sdk,
                relative_headers=(),
                header_manifests=[shard],
                target=header_surface.DEFAULT_TARGET,
            )

            self.assertEqual(merged["summary"]["header_count"], 2)
            self.assertNotIn("coverage", merged)


if __name__ == "__main__":
    unittest.main()
