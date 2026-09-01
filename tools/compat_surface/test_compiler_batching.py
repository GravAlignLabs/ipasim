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

    def test_win64_sparse_visible_carriers_preserve_full_namespace_offsets(self):
        symbols = [
            {
                "symbol": "_aggregate",
                "host_llvm_ir_name": "__ipasim_host_adapter_000000",
                "host_llvm_ir_declaration": (
                    "declare void @__ipasim_host_adapter_000000("
                    "ptr sret(%struct.__ipasim_carrier_000002_s0_ret) %0)"
                ),
                "parameters": [
                    {
                        "host_lowered_ir_type": (
                            "%struct.__ipasim_carrier_000001_s0_p0"
                        )
                    }
                ],
            }
        ]

        rewritten, adapter_offset, carrier_offset = (
            compiler_batching._canonicalize_win64_batch(
                symbols,
                adapter_offset=4,
                carrier_offset=10,
                expected_adapter_count=1,
                carrier_count=3,
            )
        )

        rendered = self.compact(rewritten)
        self.assertIn("__ipasim_host_adapter_000004", rendered)
        self.assertIn("__ipasim_carrier_000011_s4_p0", rendered)
        self.assertIn("__ipasim_carrier_000012_s4_ret", rendered)
        self.assertNotIn("__ipasim_carrier_000010_", rendered)
        self.assertEqual(adapter_offset, 5)
        self.assertEqual(carrier_offset, 13)

    def test_win64_visible_carrier_outside_generated_namespace_fails_closed(self):
        symbols = [
            {
                "symbol": "_bad",
                "host_llvm_ir_name": "__ipasim_host_adapter_000000",
                "host_llvm_ir_declaration": (
                    "declare void @__ipasim_host_adapter_000000("
                    "ptr sret(%struct.__ipasim_carrier_000003_s0_ret) %0)"
                ),
            }
        ]
        with self.assertRaisesRegex(
            compiler_batching.CompilerBatchError,
            r"carrier index 3 outside generated namespace size 3",
        ):
            compiler_batching._canonicalize_win64_batch(
                symbols,
                adapter_offset=0,
                carrier_offset=0,
                expected_adapter_count=1,
                carrier_count=3,
            )

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
