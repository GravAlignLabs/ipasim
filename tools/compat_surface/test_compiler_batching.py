import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import abi_surface
import compiler_batching
import test_abi_surface
import win64_abi_surface


class CompilerBatchingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    @staticmethod
    def compact(value):
        return json.dumps(value, separators=(",", ":"), sort_keys=False)

    def make_inventory(self, root: Path):
        fixture = test_abi_surface.AbiSurfaceTests()
        return fixture.make_inventory(root)

    def test_aapcs64_batches_are_byte_equivalent_to_single_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            baseline = abi_surface.build_abi_manifest(inventory, header_root=root)
            for batch_size in (1, 2, 3, 5):
                with self.subTest(batch_size=batch_size):
                    batched = compiler_batching.build_aapcs64_manifest(
                        inventory,
                        header_root=root,
                        batch_size=batch_size,
                    )
                    self.assertEqual(self.compact(batched), self.compact(baseline))

    def test_win64_batches_are_byte_equivalent_to_single_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            guest = abi_surface.build_abi_manifest(inventory, header_root=root)
            baseline = win64_abi_surface.build_win64_manifest(guest)
            for batch_size in (1, 2, 3, 5):
                with self.subTest(batch_size=batch_size):
                    batched = compiler_batching.build_win64_manifest(
                        guest,
                        batch_size=batch_size,
                    )
                    self.assertEqual(self.compact(batched), self.compact(baseline))

    def test_aapcs64_failure_logs_every_batch_before_failing_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            with mock.patch.object(
                compiler_batching.abi_surface,
                "build_abi_manifest",
                side_effect=abi_surface.AbiSurfaceError("synthetic clang failure"),
            ) as build:
                with self.assertRaises(compiler_batching.CompilerBatchError) as raised:
                    compiler_batching.build_aapcs64_manifest(
                        inventory,
                        header_root=root,
                        batch_size=1,
                    )

            message = str(raised.exception)
            self.assertIn("AAPCS64 completed all 8 batches with 8 failure(s)", message)
            self.assertIn(
                "AAPCS64 batch 1/8 symbols '_big_roundtrip'..'_big_roundtrip' failed: synthetic clang failure",
                message,
            )
            self.assertIn("AAPCS64 batch 8/8", message)
            self.assertEqual(build.call_count, 8)

    def test_win64_failure_names_the_exact_batch_symbol_range(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            guest = abi_surface.build_abi_manifest(inventory, header_root=root)
            with mock.patch.object(
                compiler_batching.win64_abi_surface,
                "build_win64_manifest",
                side_effect=win64_abi_surface.Win64AbiError("synthetic host clang failure"),
            ):
                with self.assertRaisesRegex(
                    compiler_batching.CompilerBatchError,
                    r"Win64 batch 1/8 symbols '_big_roundtrip'\.\.'_big_roundtrip'.*synthetic host clang failure",
                ):
                    compiler_batching.build_win64_manifest(
                        guest,
                        batch_size=1,
                    )

    def test_invalid_batch_sizes_fail_closed(self):
        for value in (0, -1, True, 1.5):
            with self.subTest(value=value):
                with self.assertRaises(compiler_batching.CompilerBatchError):
                    compiler_batching.build_aapcs64_manifest(
                        {"symbols": []},
                        batch_size=value,
                        header_root=Path("."),
                    )


if __name__ == "__main__":
    unittest.main()
