import json
import unittest

import bridge_plan


def guest_regs(*names):
    bank = "simd" if names and names[0].startswith("v") else "gpr"
    return {"kind": "registers", "bank": bank, "registers": list(names)}


def host_reg(name):
    return {
        "kind": "register",
        "bank": "simd" if name.startswith("xmm") else "gpr",
        "register": name,
    }


def transfer(
    kind,
    source_index,
    carrier_ir_type,
    guest_location,
    host_ir_type,
    host_location,
    complexity,
    *,
    attrs=None,
    indirect=False,
):
    result = {
        "kind": kind,
        "source_index": source_index,
        "carrier_ir_type": carrier_ir_type,
        "guest_location": guest_location,
        "host_lowered_ir_type": host_ir_type,
        "host_llvm_attributes": attrs or [],
        "host_location": host_location,
        "transfer_complexity": complexity,
    }
    if indirect:
        result["indirect_source_aggregate"] = True
    return result


def manifest():
    return {
        "schema_version": 1,
        "kind": "win64-carrier-abi-surface",
        "guest_target": "arm64-apple-ios16.0",
        "host_target": "x86_64-pc-windows-msvc",
        "summary": {},
        "symbols": [
            {
                "symbol": "_scalar",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [
                    transfer(
                        "source-parameter", 0, "i32", guest_regs("x0"),
                        "i32", host_reg("rcx"), "scalar-register-transfer",
                    ),
                    transfer(
                        "source-parameter", 1, "double", guest_regs("v0"),
                        "double", host_reg("xmm1"), "scalar-register-transfer",
                    ),
                    transfer(
                        "source-parameter", 2, "ptr", guest_regs("x1"),
                        "ptr", host_reg("r8"), "scalar-register-transfer",
                    ),
                ],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "i32",
                    "guest_location": guest_regs("x0"),
                    "host_carrier_result": {"kind": "register", "register": "rax"},
                    "transfer_complexity": "scalar-register-transfer",
                },
            },
            {
                "symbol": "_pair",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [
                    transfer(
                        "source-parameter", 0, "[2 x i64]", guest_regs("x0", "x1"),
                        "ptr", host_reg("rdx"), "repack-or-materialize",
                    )
                ],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "[2 x i64]",
                    "guest_location": guest_regs("x0", "x1"),
                    "host_carrier_result": {
                        "kind": "indirect-result", "location": host_reg("rcx")
                    },
                    "transfer_complexity": "repack-or-materialize",
                },
            },
            {
                "symbol": "_hfa",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [
                    transfer(
                        "source-parameter", 0, "[2 x float]", guest_regs("v0", "v1"),
                        "i64", host_reg("rcx"), "repack-or-materialize",
                    )
                ],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "[2 x float]",
                    "guest_location": guest_regs("v0", "v1"),
                    "host_carrier_result": {"kind": "register", "register": "rax"},
                    "transfer_complexity": "repack-or-materialize",
                },
            },
            {
                "symbol": "_big",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [
                    transfer(
                        "source-parameter", 0, "ptr", guest_regs("x0"),
                        "ptr", host_reg("rdx"), "scalar-register-transfer",
                        indirect=True,
                    )
                ],
                "return": {
                    "kind": "guest-indirect-result-pointer",
                    "guest_location": {"kind": "indirect-result", "register": "x8"},
                    "host_carrier_result": {"kind": "void"},
                    "result_pointer_transfer": transfer(
                        "guest-result-pointer", None, "ptr",
                        {"kind": "indirect-result", "register": "x8"},
                        "ptr", host_reg("rcx"), "pointer-register-transfer",
                    ),
                },
            },
            {
                "symbol": "_guest_stack",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [
                    transfer(
                        "source-parameter", 0, "i64",
                        {"kind": "stack", "bank": "gpr", "stack_offset": None},
                        "i64", host_reg("rcx"), "stack-transfer",
                    )
                ],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "i64",
                    "guest_location": guest_regs("x0"),
                    "host_carrier_result": {"kind": "register", "register": "rax"},
                    "transfer_complexity": "scalar-register-transfer",
                },
            },
            {
                "symbol": "_host_stack_only",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [
                    transfer(
                        "source-parameter", 0, "i64", guest_regs("x0"), "i64",
                        {"kind": "stack", "stack_offset": None}, "stack-transfer",
                    )
                ],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "i64",
                    "guest_location": guest_regs("x0"),
                    "host_carrier_result": {"kind": "register", "register": "rax"},
                    "transfer_complexity": "scalar-register-transfer",
                },
            },
            {
                "symbol": "_callback",
                "cross_abi_status": "inherited-runtime-boundary",
                "reasons": ["callback runtime"],
                "parameters": [],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "void",
                    "guest_location": {"kind": "void"},
                    "host_carrier_result": {"kind": "void"},
                },
            },
        ],
    }


class BridgePlanTests(unittest.TestCase):
    @staticmethod
    def by_symbol(plan):
        return {item["symbol"]: item for item in plan["symbols"]}

    def test_scalar_descriptor_preserves_pointer_policy(self):
        scalar = self.by_symbol(bridge_plan.build_bridge_plan(manifest()))["_scalar"]
        self.assertEqual(scalar["plan_status"], "libffi-descriptor-candidate")
        self.assertEqual(
            [item["ffi_type"]["ffi_type"] for item in scalar["arguments"]],
            ["ffi_type_uint32", "ffi_type_double", "ffi_type_pointer"],
        )
        self.assertEqual(
            scalar["arguments"][2]["ffi_type"]["pointer_policy"],
            "opaque-guest-address-requires-runtime-validation",
        )
        self.assertEqual(scalar["result"]["ffi_type"]["ffi_type"], "ffi_type_uint32")

    def test_pair_and_hfa_use_struct_descriptors_and_lane_widths(self):
        items = self.by_symbol(bridge_plan.build_bridge_plan(manifest()))
        pair = items["_pair"]
        pair_type = pair["arguments"][0]["ffi_type"]
        self.assertEqual(pair_type["kind"], "struct")
        self.assertEqual(len(pair_type["elements"]), 2)
        self.assertEqual(pair["arguments"][0]["guest_capture"]["element_width_bytes"], 8)
        self.assertEqual(pair["result"]["guest_commit"]["operation"], "commit-register-elements")

        hfa = items["_hfa"]
        self.assertEqual(hfa["arguments"][0]["guest_capture"]["element_width_bytes"], 4)
        self.assertEqual(
            [e["ffi_type"] for e in hfa["arguments"][0]["ffi_type"]["elements"]],
            ["ffi_type_float", "ffi_type_float"],
        )
        self.assertEqual(hfa["arguments"][0]["host_abi_evidence"]["lowered_ir_type"], "i64")

    def test_guest_x8_result_pointer_becomes_first_ffi_argument(self):
        big = self.by_symbol(bridge_plan.build_bridge_plan(manifest()))["_big"]
        self.assertEqual(big["libffi"]["argument_count"], 2)
        self.assertEqual(big["arguments"][0]["kind"], "guest-result-pointer")
        self.assertEqual(big["arguments"][0]["guest_capture"]["operation"], "capture-result-pointer")
        self.assertEqual(big["arguments"][0]["guest_capture"]["guest_location"]["register"], "x8")
        self.assertEqual(big["result"]["ffi_type"]["ffi_type"], "ffi_type_void")
        self.assertEqual(
            big["result"]["guest_commit"]["operation"],
            "callee-writes-through-result-pointer",
        )

    def test_unknown_guest_stack_offset_blocks_descriptor_execution_plan(self):
        item = self.by_symbol(bridge_plan.build_bridge_plan(manifest()))["_guest_stack"]
        self.assertEqual(item["plan_status"], "needs-guest-layout")
        self.assertIn("needs-guest-stack-layout", " ".join(item["reasons"]))

    def test_host_stack_offset_is_not_required_when_libffi_owns_host_calling(self):
        item = self.by_symbol(bridge_plan.build_bridge_plan(manifest()))["_host_stack_only"]
        self.assertEqual(item["plan_status"], "libffi-descriptor-candidate")
        self.assertEqual(item["arguments"][0]["host_abi_evidence"]["location"]["kind"], "stack")

    def test_runtime_boundaries_and_unsupported_carriers_remain_visible(self):
        callback = self.by_symbol(bridge_plan.build_bridge_plan(manifest()))["_callback"]
        self.assertEqual(callback["plan_status"], "inherited-boundary")

        changed = manifest()
        changed["symbols"].append(
            {
                "symbol": "_vector",
                "cross_abi_status": "cross-abi-adapter-candidate",
                "reasons": [],
                "parameters": [],
                "return": {
                    "kind": "value-result",
                    "carrier_ir_type": "<4 x float>",
                    "guest_location": guest_regs("v0"),
                    "host_carrier_result": {"kind": "register", "register": "xmm0"},
                },
            }
        )
        vector = self.by_symbol(bridge_plan.build_bridge_plan(changed))["_vector"]
        self.assertEqual(vector["plan_status"], "needs-ffi-type-or-layout")
        self.assertIn("not proven for vendored libffi", " ".join(vector["reasons"]))

    def test_output_is_deterministic_and_schema_is_checked(self):
        first = bridge_plan.build_bridge_plan(manifest())
        second = bridge_plan.build_bridge_plan(manifest())
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True))
        invalid = manifest()
        invalid["kind"] = "wrong"
        with self.assertRaises(bridge_plan.BridgePlanError):
            bridge_plan.build_bridge_plan(invalid)


if __name__ == "__main__":
    unittest.main()
