import json
import shutil
import unittest

import win64_abi_surface


def regs(*names):
    if not names:
        return {"kind": "void"}
    bank = "simd" if names[0].startswith("v") else "gpr"
    return {
        "kind": "registers",
        "bank": bank,
        "registers": list(names),
    }


def param(index, ir_type, location, *, indirect=False):
    return {
        "index": index,
        "lowered_ir_type": ir_type,
        "location": location,
        "indirect_source_aggregate": indirect,
    }


def symbol(
    name,
    ret_type,
    ret_location,
    params,
    *,
    status="generated-bridge-candidate",
    reasons=None,
):
    return {
        "symbol": name,
        "c_name": name.lstrip("_"),
        "source_header": "fixture.h",
        "bridge_status": status,
        "bridge_reasons": reasons or [],
        "return": {
            "lowered_ir_type": ret_type,
            "location": ret_location,
        },
        "parameters": params,
    }


def guest_manifest():
    pressure_params = [
        param(index, "i64", regs(f"x{index}"))
        for index in range(5)
    ]
    return {
        "schema_version": 1,
        "kind": "aapcs64-abi-surface",
        "target": "arm64-apple-ios16.0",
        "summary": {},
        "symbols": [
            symbol(
                "_scalar",
                "i32",
                regs("x0"),
                [
                    param(0, "i32", regs("x0")),
                    param(1, "double", regs("v0")),
                    param(2, "ptr", regs("x1")),
                ],
            ),
            symbol(
                "_pair",
                "[2 x i64]",
                regs("x0", "x1"),
                [param(0, "[2 x i64]", regs("x0", "x1"))],
            ),
            symbol(
                "_hfa",
                "[2 x float]",
                regs("v0", "v1"),
                [param(0, "[2 x float]", regs("v0", "v1"))],
            ),
            symbol(
                "_big",
                "void",
                {
                    "kind": "indirect-result",
                    "register": "x8",
                    "llvm_parameter": "ptr sret",
                },
                [param(0, "ptr", regs("x0"), indirect=True)],
            ),
            symbol(
                "_pressure",
                "i64",
                regs("x0"),
                pressure_params,
            ),
            symbol(
                "_callback",
                "void",
                {"kind": "void"},
                [],
                status="callback-runtime",
                reasons=["function pointer requires callback runtime"],
            ),
        ],
    }


class Win64AbiSurfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    @staticmethod
    def by_symbol(manifest):
        return {
            item["symbol"]: item
            for item in manifest["symbols"]
        }

    def test_scalar_win64_slots_are_positional(self):
        scalar = self.by_symbol(
            win64_abi_surface.build_win64_manifest(guest_manifest())
        )["_scalar"]
        self.assertEqual(
            [
                item["host_location"]["register"]
                for item in scalar["parameters"]
            ],
            ["rcx", "xmm1", "r8"],
        )
        self.assertEqual(
            scalar["return"]["host_carrier_result"]["register"],
            "rax",
        )
        self.assertEqual(
            scalar["cross_abi_status"],
            "cross-abi-adapter-candidate",
        )

    def test_aggregate_cross_abi_differences_are_visible(self):
        items = self.by_symbol(
            win64_abi_surface.build_win64_manifest(guest_manifest())
        )
        pair = items["_pair"]
        self.assertEqual(
            pair["return"]["host_carrier_result"]["kind"],
            "indirect-result",
        )
        self.assertEqual(
            pair["return"]["host_carrier_result"]["location"][
                "register"
            ],
            "rcx",
        )
        self.assertEqual(
            pair["parameters"][0]["host_location"]["register"],
            "rdx",
        )
        self.assertEqual(
            pair["parameters"][0]["host_lowered_ir_type"],
            "ptr",
        )

        hfa = items["_hfa"]
        self.assertEqual(
            hfa["parameters"][0]["host_lowered_ir_type"],
            "i64",
        )
        self.assertEqual(
            hfa["parameters"][0]["host_location"]["register"],
            "rcx",
        )
        self.assertEqual(
            hfa["return"]["host_carrier_result"]["register"],
            "rax",
        )
        self.assertEqual(
            hfa["parameters"][0]["transfer_complexity"],
            "repack-or-materialize",
        )

    def test_guest_indirect_result_becomes_explicit_carrier_pointer(self):
        big = self.by_symbol(
            win64_abi_surface.build_win64_manifest(guest_manifest())
        )["_big"]
        result = big["return"]
        self.assertEqual(
            result["kind"],
            "guest-indirect-result-pointer",
        )
        self.assertEqual(
            result["result_pointer_transfer"]["guest_location"][
                "register"
            ],
            "x8",
        )
        self.assertEqual(
            result["result_pointer_transfer"]["host_location"][
                "register"
            ],
            "rcx",
        )
        self.assertEqual(
            big["parameters"][0]["host_location"]["register"],
            "rdx",
        )
        self.assertTrue(
            big["parameters"][0]["indirect_source_aggregate"]
        )

    def test_fifth_win64_slot_is_stack_without_fake_offset(self):
        pressure = self.by_symbol(
            win64_abi_surface.build_win64_manifest(guest_manifest())
        )["_pressure"]
        self.assertEqual(
            [
                item["host_location"].get("register")
                for item in pressure["parameters"][:4]
            ],
            ["rcx", "rdx", "r8", "r9"],
        )
        self.assertEqual(
            pressure["parameters"][4]["host_location"]["kind"],
            "stack",
        )
        self.assertIsNone(
            pressure["parameters"][4]["host_location"][
                "stack_offset"
            ]
        )

    def test_runtime_boundaries_are_inherited_not_reclassified(self):
        callback = self.by_symbol(
            win64_abi_surface.build_win64_manifest(guest_manifest())
        )["_callback"]
        self.assertEqual(
            callback["cross_abi_status"],
            "inherited-runtime-boundary",
        )
        self.assertIn(
            "callback runtime",
            " ".join(callback["reasons"]),
        )

    def test_unresolved_named_carrier_is_visible(self):
        manifest = guest_manifest()
        manifest["symbols"].append(
            symbol(
                "_named",
                "%struct.Unknown",
                regs("x0"),
                [],
            )
        )
        named = self.by_symbol(
            win64_abi_surface.build_win64_manifest(manifest)
        )["_named"]
        self.assertEqual(
            named["cross_abi_status"],
            "needs-carrier-type",
        )
        self.assertIn(
            "unresolved named LLVM type",
            " ".join(named["reasons"]),
        )

    def test_output_is_deterministic_and_target_is_checked(self):
        first = win64_abi_surface.build_win64_manifest(
            guest_manifest()
        )
        second = win64_abi_surface.build_win64_manifest(
            guest_manifest()
        )
        self.assertEqual(
            json.dumps(first, sort_keys=True),
            json.dumps(second, sort_keys=True),
        )
        with self.assertRaises(win64_abi_surface.Win64AbiError):
            win64_abi_surface.build_win64_manifest(
                guest_manifest(),
                host_target="aarch64-pc-windows-msvc",
            )


if __name__ == "__main__":
    unittest.main()
