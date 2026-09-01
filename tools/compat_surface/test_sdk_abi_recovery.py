import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import compiler_batching
import sdk_abi_recovery


BUILTIN_INT = {"kind": "builtin", "name": "int"}
BUILTIN_UINT = {"kind": "builtin", "name": "unsigned int"}


def _inventory(relative: str) -> dict:
    signature = {
        "symbol": "_LeafThing",
        "names": ["LeafThing"],
        "function_type_spellings": [],
        "calling_convention": "cdecl",
        "variadic": False,
        "prototype": True,
        "return_type": BUILTIN_INT,
        "parameters": [
            {
                "index": 0,
                "names": ["value"],
                "spellings": ["page_number_t"],
                "type": BUILTIN_UINT,
            }
        ],
        "sources": [
            {
                "header": relative,
                "line": 1,
                "column": 1,
            }
        ],
    }
    return {
        "schema_version": 1,
        "kind": "typed-compatibility-inventory",
        "targets": {
            "clang": "arm64-apple-ios16.0",
            "tapi": "arm64-ios",
        },
        "summary": {},
        "symbols": [
            {
                "symbol": "_LeafThing",
                "required_by": [],
                "requirement_count": 0,
                "sdk_direct_exports": [],
                "signature": signature,
            }
        ],
        "requirements": [],
    }


class SdkAbiRecoveryTests(unittest.TestCase):
    def test_direct_include_instruction_is_resolved_from_clang_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            owner = sdk_root / "usr" / "include" / "net" / "in.h"
            leaf = sdk_root / "usr" / "include" / "net6" / "in6.h"
            owner.parent.mkdir(parents=True)
            leaf.parent.mkdir(parents=True)
            owner.write_text("#define TEST_OWNER_ENTERED 1\n", encoding="utf-8")
            leaf.write_text("int LeafThing(unsigned int value);\n", encoding="utf-8")
            relative = leaf.relative_to(sdk_root).as_posix()
            diagnostic = (
                f"<SDKROOT>/{relative}:1:2: error: "
                '"do not include net6/in6.h directly, include net/in.h. "\n'
            )

            recovered = sdk_abi_recovery.diagnostic_recovery_candidates(
                diagnostic,
                header_root=sdk_root,
                sdk_root=sdk_root,
                source_headers=[relative],
            )

            self.assertEqual(
                recovered[relative].explicit,
                (owner.resolve(),),
            )
            self.assertEqual(recovered[relative].providers, ())

    def test_unknown_type_uses_sdk_typedef_provider_as_recovery_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            include = sdk_root / "usr" / "include"
            include.mkdir(parents=True)
            provider = include / "Provider.h"
            leaf = include / "Leaf.h"
            provider.write_text(
                "typedef unsigned int page_number_t;\n",
                encoding="utf-8",
            )
            leaf.write_text(
                "int LeafThing(page_number_t value);\n",
                encoding="utf-8",
            )
            relative = leaf.relative_to(sdk_root).as_posix()
            diagnostic = (
                f"<SDKROOT>/{relative}:1:15: error: "
                "unknown type name 'page_number_t'\n"
            )

            recovered = sdk_abi_recovery.diagnostic_recovery_candidates(
                diagnostic,
                header_root=sdk_root,
                sdk_root=sdk_root,
                source_headers=[relative],
            )

            self.assertEqual(recovered[relative].explicit, ())
            self.assertEqual(
                recovered[relative].providers[0],
                provider.resolve(),
            )

    @unittest.skipUnless(shutil.which("clang"), "clang is required")
    def test_complete_aapcs64_pass_retries_with_proven_typedef_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            include = sdk_root / "usr" / "include"
            include.mkdir(parents=True)
            (include / "Provider.h").write_text(
                "typedef unsigned int page_number_t;\n",
                encoding="utf-8",
            )
            leaf = include / "Leaf.h"
            leaf.write_text(
                "int LeafThing(page_number_t value);\n",
                encoding="utf-8",
            )
            relative = leaf.relative_to(sdk_root).as_posix()

            manifest = sdk_abi_recovery.build_aapcs64_manifest(
                _inventory(relative),
                header_root=sdk_root,
                sdk_root=sdk_root,
                clang="clang",
                extra_args=("-O1",),
                timeout_seconds=30,
                batch_size=8,
            )

            self.assertEqual(manifest["summary"]["typed_symbol_count"], 1)
            self.assertEqual(manifest["symbols"][0]["symbol"], "_LeafThing")
            self.assertEqual(manifest["symbols"][0]["llvm_ir_name"], "LeafThing")

    def test_no_diagnostic_evidence_rethrows_original_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "sdk"
            include = sdk_root / "usr" / "include"
            include.mkdir(parents=True)
            leaf = include / "Leaf.h"
            leaf.write_text(
                "int LeafThing(unsigned int value);\n",
                encoding="utf-8",
            )
            relative = leaf.relative_to(sdk_root).as_posix()
            failure = compiler_batching.CompilerBatchError(
                "AAPCS64 completed all 1 batches with 1 failure(s):\n"
                "AAPCS64 batch 1/1 symbols '_LeafThing'..'_LeafThing' failed: "
                "Clang ABI probe failed with exit code 1:\n"
                f"<SDKROOT>/{relative}:1:1: error: synthetic failure without SDK evidence"
            )

            with mock.patch.object(
                sdk_abi_recovery.sdk_abi_context,
                "build_aapcs64_manifest",
                side_effect=failure,
            ):
                with self.assertRaises(compiler_batching.CompilerBatchError) as raised:
                    sdk_abi_recovery.build_aapcs64_manifest(
                        _inventory(relative),
                        header_root=sdk_root,
                        sdk_root=sdk_root,
                        clang="clang",
                        timeout_seconds=30,
                        batch_size=8,
                    )

            self.assertIs(raised.exception, failure)


if __name__ == "__main__":
    unittest.main()
