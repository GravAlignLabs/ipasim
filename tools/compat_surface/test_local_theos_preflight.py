import json
import tempfile
import unittest
from pathlib import Path

import local_theos_preflight


class LocalTheosPreflightTests(unittest.TestCase):
    def test_checked_in_baseline_matches_local_ci_constants(self):
        baseline = local_theos_preflight.load_baseline()
        params = baseline["ci_parameters_mirrored"]
        self.assertEqual(
            params["header_shard_count"],
            local_theos_preflight.HEADER_SHARD_COUNT,
        )
        self.assertEqual(
            params["header_jobs_per_shard"],
            local_theos_preflight.HEADER_JOBS_PER_SHARD,
        )
        self.assertEqual(
            params["header_progress_every"],
            local_theos_preflight.HEADER_PROGRESS_EVERY,
        )
        self.assertEqual(
            params["clang_timeout_seconds"],
            local_theos_preflight.CLANG_TIMEOUT_SECONDS,
        )
        self.assertEqual(
            params["header_shard_wall_timeout_minutes"] * 60,
            local_theos_preflight.SHARD_WALL_TIMEOUT_SECONDS,
        )
        self.assertEqual(
            params["full_preflight_wall_timeout_minutes"] * 60,
            local_theos_preflight.FULL_PREFLIGHT_WALL_TIMEOUT_SECONDS,
        )
        self.assertEqual(
            params["compiler_batch_size"],
            local_theos_preflight.COMPILER_BATCH_SIZE,
        )
        self.assertEqual(
            baseline["theos_sdk"]["commit"],
            local_theos_preflight.THEOS_SDK_COMMIT,
        )
        self.assertEqual(
            baseline["theos_sdk"]["sdk"],
            local_theos_preflight.SDK_RELATIVE_PATH.as_posix(),
        )

    def test_checked_in_baseline_detects_workflow_drift(self):
        baseline = local_theos_preflight.load_baseline()
        current = local_theos_preflight.workflow_blob_sha()
        if current is None:
            self.skipTest("git checkout metadata is unavailable")
        self.assertEqual(current, baseline["ci_workflow_blob_sha"])

    def test_cached_shard_requires_exact_coverage_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "header-shard-7.json"
            manifest = {
                "schema_version": 1,
                "kind": "header-signature-surface",
                "target": local_theos_preflight.HEADER_TARGET,
                "coverage": {
                    "schema_version": 1,
                    "strategy": "sorted-round-robin",
                    "shard_count": local_theos_preflight.HEADER_SHARD_COUNT,
                    "shard_index": 7,
                    "sdk_header_count": 5118,
                    "headers": ["usr/include/example.h"],
                },
            }
            path.write_text(json.dumps(manifest), encoding="utf-8")
            self.assertTrue(local_theos_preflight.valid_cached_shard(path, 7))
            self.assertFalse(local_theos_preflight.valid_cached_shard(path, 8))

            manifest["coverage"]["shard_count"] = 16
            path.write_text(json.dumps(manifest), encoding="utf-8")
            self.assertFalse(local_theos_preflight.valid_cached_shard(path, 7))

    def test_cache_keys_are_stable_for_current_checkout(self):
        self.assertEqual(
            local_theos_preflight.header_cache_key(),
            local_theos_preflight.header_cache_key(),
        )
        self.assertEqual(
            local_theos_preflight.pipeline_cache_key(),
            local_theos_preflight.pipeline_cache_key(),
        )
        self.assertNotEqual(
            local_theos_preflight.header_cache_key(),
            local_theos_preflight.pipeline_cache_key(),
        )


if __name__ == "__main__":
    unittest.main()
