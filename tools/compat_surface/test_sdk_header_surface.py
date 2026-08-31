import json
import shutil
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import header_surface
import sdk_header_surface


class SdkHeaderSurfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    @staticmethod
    def compact(value):
        return json.dumps(value, separators=(",", ":"), sort_keys=False)

    @staticmethod
    def make_headers(root: Path):
        sources = {
            "a.h": "int alpha(int value);\n",
            "b.h": "double beta(double value);\n",
            "c.h": (
                "struct Pair { long a; long b; };\n"
                "struct Pair pair_roundtrip(struct Pair value);\n"
            ),
        }
        inputs = []
        for name, source in sources.items():
            path = root / name
            path.write_text(source, encoding="utf-8")
            inputs.append((path, name))
        return inputs

    def sequential_manifest(self, inputs, root):
        signatures = []
        stats = []
        for path, display in inputs:
            current, current_stats = header_surface.analyze_header(
                path,
                display,
                sdk_root=root,
            )
            signatures.extend(current)
            stats.append(current_stats)
        return header_surface.build_manifest(
            signatures,
            target=header_surface.DEFAULT_TARGET,
            headers=[display for _, display in inputs],
            stats=stats,
        )

    def test_parallel_jobs_are_byte_equivalent_to_sequential_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = self.make_headers(root)
            baseline = self.sequential_manifest(inputs, root)
            for jobs in (1, 2, 4):
                with self.subTest(jobs=jobs):
                    parallel = sdk_header_surface.build_parallel_manifest(
                        inputs,
                        jobs=jobs,
                        sdk_root=root,
                    )
                    self.assertEqual(
                        self.compact(parallel),
                        self.compact(baseline),
                    )

    def test_completion_order_does_not_change_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = self.make_headers(root)
            baseline = self.sequential_manifest(inputs, root)
            original = header_surface.analyze_header
            delays = {"a.h": 0.06, "b.h": 0.03, "c.h": 0.0}

            def delayed(path, display, **kwargs):
                time.sleep(delays[display])
                return original(path, display, **kwargs)

            with mock.patch.object(
                sdk_header_surface.header_surface,
                "analyze_header",
                side_effect=delayed,
            ):
                parallel = sdk_header_surface.build_parallel_manifest(
                    inputs,
                    jobs=3,
                    sdk_root=root,
                )
            self.assertEqual(self.compact(parallel), self.compact(baseline))

    def test_multiple_concurrent_failures_report_earliest_input_header(self):
        inputs = [
            (Path("a.h"), "a.h"),
            (Path("b.h"), "b.h"),
            (Path("c.h"), "c.h"),
        ]

        def synthetic(path, display, **kwargs):
            del path, kwargs
            if display == "a.h":
                time.sleep(0.04)
                return [], {"skipped_cxx": 0, "skipped_static": 0, "declarations": 0}
            if display == "b.h":
                time.sleep(0.03)
                raise header_surface.HeaderParseError("b failed")
            raise header_surface.HeaderParseError("c failed first in wall-clock time")

        with mock.patch.object(
            sdk_header_surface.header_surface,
            "analyze_header",
            side_effect=synthetic,
        ):
            with self.assertRaisesRegex(
                sdk_header_surface.SdkHeaderSurfaceError,
                r"header 2/3 'b\.h' failed: b failed",
            ):
                sdk_header_surface.build_parallel_manifest(inputs, jobs=3)

    def test_invalid_job_counts_fail_closed(self):
        for jobs in (0, -1, True, 1.5, sdk_header_surface.MAX_HEADER_JOBS + 1):
            with self.subTest(jobs=jobs):
                with self.assertRaises(sdk_header_surface.SdkHeaderSurfaceError):
                    sdk_header_surface.build_parallel_manifest(
                        [(Path("a.h"), "a.h")],
                        jobs=jobs,
                    )

    def test_cli_scans_sdk_root_and_keeps_paths_relative(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_headers(root)
            output = root / "headers.json"
            code = sdk_header_surface.main(
                [
                    "--sdk-root",
                    str(root),
                    "--jobs",
                    "2",
                    "--output",
                    str(output),
                ]
            )
            self.assertEqual(code, 0)
            rendered = output.read_text(encoding="utf-8")
            self.assertNotIn(str(root), rendered)
            manifest = json.loads(rendered)
            self.assertEqual(manifest["summary"]["header_count"], 3)
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_alpha", "_beta", "_pair_roundtrip"],
            )


if __name__ == "__main__":
    unittest.main()
