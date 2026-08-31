import json
import shutil
import tempfile
import unittest
from pathlib import Path

import abi_surface


HEADER = r'''
typedef unsigned long size_t;
struct Pair { long a; long b; };
struct HFA { float a; float b; };
struct Big { long a; long b; long c; long d; long e; };

int scalar(int a, double b, void *p);
struct Pair pair_roundtrip(struct Pair value);
struct HFA hfa_roundtrip(struct HFA value);
struct Big big_roundtrip(struct Big value);
int variadic_fn(int tag, ...);
void callback_fn(void (*fn)(int));
int old_style();
long pressure(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9,
              double f0, double f1, double f2, double f3, double f4, double f5, double f6, double f7, double f8, double f9);
'''


def sig(symbol, name, header, ret, params, *, variadic=False, prototype=True):
    return {
        "symbol": symbol,
        "names": [name],
        "function_type_spellings": [],
        "calling_convention": "cdecl",
        "variadic": variadic,
        "prototype": prototype,
        "return_type": ret,
        "parameters": [
            {
                "index": index,
                "names": [],
                "spellings": [],
                "type": value,
            }
            for index, value in enumerate(params)
        ],
        "sources": [
            {
                "header": header,
                "line": 1,
                "column": 1,
            }
        ],
    }


BUILTIN_INT = {"kind": "builtin", "name": "int"}
BUILTIN_LONG = {"kind": "builtin", "name": "long"}
BUILTIN_DOUBLE = {"kind": "builtin", "name": "double"}
BUILTIN_VOID = {"kind": "builtin", "name": "void"}
PTR_VOID = {"kind": "pointer", "pointee": BUILTIN_VOID}
PAIR = {"kind": "record", "name": "struct Pair"}
HFA = {"kind": "record", "name": "struct HFA"}
BIG = {"kind": "record", "name": "struct Big"}
CALLBACK = {
    "kind": "pointer",
    "pointee": {
        "kind": "function",
        "prototype": True,
        "calling_convention": "cdecl",
        "variadic": False,
        "return": BUILTIN_VOID,
        "parameters": [BUILTIN_INT],
    },
}


class AbiSurfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    def make_inventory(self, root: Path):
        header = root / "abi_fixture.h"
        header.write_text(HEADER, encoding="utf-8")
        signatures = [
            sig(
                "_scalar",
                "scalar",
                header.name,
                BUILTIN_INT,
                [BUILTIN_INT, BUILTIN_DOUBLE, PTR_VOID],
            ),
            sig(
                "_pair_roundtrip",
                "pair_roundtrip",
                header.name,
                PAIR,
                [PAIR],
            ),
            sig(
                "_hfa_roundtrip",
                "hfa_roundtrip",
                header.name,
                HFA,
                [HFA],
            ),
            sig(
                "_big_roundtrip",
                "big_roundtrip",
                header.name,
                BIG,
                [BIG],
            ),
            sig(
                "_variadic_fn",
                "variadic_fn",
                header.name,
                BUILTIN_INT,
                [BUILTIN_INT],
                variadic=True,
            ),
            sig(
                "_callback_fn",
                "callback_fn",
                header.name,
                BUILTIN_VOID,
                [CALLBACK],
            ),
            sig(
                "_old_style",
                "old_style",
                header.name,
                BUILTIN_INT,
                [],
                prototype=False,
            ),
            sig(
                "_pressure",
                "pressure",
                header.name,
                BUILTIN_LONG,
                [BUILTIN_LONG] * 10 + [BUILTIN_DOUBLE] * 10,
            ),
        ]
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
                    "symbol": item["symbol"],
                    "required_by": ["fixture.dylib"],
                    "requirement_count": 1,
                    "sdk_direct_exports": [],
                    "signature": item,
                }
                for item in signatures
            ],
            "requirements": [],
        }

    @staticmethod
    def by_symbol(manifest):
        return {
            item["symbol"]: item
            for item in manifest["symbols"]
        }

    def test_clang_lowering_drives_register_plan(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = abi_surface.build_abi_manifest(
                self.make_inventory(root),
                header_root=root,
            )
            items = self.by_symbol(manifest)

            scalar = items["_scalar"]
            self.assertEqual(
                scalar["return"]["location"]["registers"],
                ["x0"],
            )
            self.assertEqual(
                scalar["parameters"][0]["location"]["registers"],
                ["x0"],
            )
            self.assertEqual(
                scalar["parameters"][1]["location"]["registers"],
                ["v0"],
            )
            self.assertEqual(
                scalar["parameters"][2]["location"]["registers"],
                ["x1"],
            )
            self.assertEqual(
                scalar["bridge_status"],
                "generated-bridge-candidate",
            )

            pair = items["_pair_roundtrip"]
            self.assertEqual(
                pair["parameters"][0]["lowered_ir_type"],
                "[2 x i64]",
            )
            self.assertEqual(
                pair["parameters"][0]["location"]["registers"],
                ["x0", "x1"],
            )
            self.assertEqual(
                pair["return"]["location"]["registers"],
                ["x0", "x1"],
            )

            hfa = items["_hfa_roundtrip"]
            self.assertEqual(
                hfa["parameters"][0]["lowered_ir_type"],
                "[2 x float]",
            )
            self.assertEqual(
                hfa["parameters"][0]["location"]["registers"],
                ["v0", "v1"],
            )
            self.assertEqual(
                hfa["return"]["location"]["registers"],
                ["v0", "v1"],
            )

    def test_indirect_result_and_indirect_aggregate_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = self.by_symbol(
                abi_surface.build_abi_manifest(
                    self.make_inventory(root),
                    header_root=root,
                )
            )
            big = items["_big_roundtrip"]
            self.assertEqual(
                big["return"]["location"]["kind"],
                "indirect-result",
            )
            self.assertEqual(
                big["return"]["location"]["register"],
                "x8",
            )
            self.assertTrue(
                big["parameters"][0]["indirect_source_aggregate"]
            )
            self.assertEqual(
                big["parameters"][0]["location"]["registers"],
                ["x0"],
            )

    def test_runtime_boundaries_are_not_claimed_as_generic_bridges(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = self.by_symbol(
                abi_surface.build_abi_manifest(
                    self.make_inventory(root),
                    header_root=root,
                )
            )
            self.assertEqual(
                items["_variadic_fn"]["bridge_status"],
                "variadic-runtime",
            )
            self.assertEqual(
                items["_callback_fn"]["bridge_status"],
                "callback-runtime",
            )
            self.assertIn(
                "callback trampoline",
                " ".join(items["_callback_fn"]["bridge_reasons"]),
            )
            self.assertEqual(
                items["_old_style"]["bridge_status"],
                "unsupported-no-prototype",
            )

    def test_register_exhaustion_is_visible_without_fake_stack_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pressure = self.by_symbol(
                abi_surface.build_abi_manifest(
                    self.make_inventory(root),
                    header_root=root,
                )
            )["_pressure"]
            for index in range(8):
                self.assertEqual(
                    pressure["parameters"][index]["location"]["registers"],
                    [f"x{index}"],
                )
            self.assertEqual(
                pressure["parameters"][8]["location"]["kind"],
                "stack",
            )
            self.assertIsNone(
                pressure["parameters"][8]["location"]["stack_offset"]
            )
            for index in range(8):
                self.assertEqual(
                    pressure["parameters"][10 + index]["location"]["registers"],
                    [f"v{index}"],
                )
            self.assertEqual(
                pressure["parameters"][18]["location"]["kind"],
                "stack",
            )

    def test_output_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            first = abi_surface.build_abi_manifest(
                inventory,
                header_root=root,
            )
            second = abi_surface.build_abi_manifest(
                inventory,
                header_root=root,
            )
            self.assertEqual(
                json.dumps(first, sort_keys=True),
                json.dumps(second, sort_keys=True),
            )

    def test_untyped_inventory_symbols_are_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            inventory["symbols"].append(
                {
                    "symbol": "_unknown",
                    "signature": None,
                }
            )
            manifest = abi_surface.build_abi_manifest(
                inventory,
                header_root=root,
            )
            self.assertNotIn(
                "_unknown",
                self.by_symbol(manifest),
            )

    def test_invalid_target_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = self.make_inventory(root)
            inventory["targets"]["clang"] = (
                "x86_64-apple-ios16.0-simulator"
            )
            with self.assertRaises(abi_surface.AbiSurfaceError):
                abi_surface.build_abi_manifest(
                    inventory,
                    header_root=root,
                )


if __name__ == "__main__":
    unittest.main()
