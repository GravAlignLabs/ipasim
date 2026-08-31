import copy
import json
import tempfile
import unittest
from pathlib import Path

import runtime_adapter_table


HERE = Path(__file__).resolve().parent
FIXTURE = HERE / "fixtures" / "runtime_adapter_bridge_plan.json"
GENERATED_CPP = HERE / "fixtures" / "runtime_adapter_fixture.inc"


class RuntimeAdapterTableTests(unittest.TestCase):
    def load_fixture(self):
        return json.loads(FIXTURE.read_text(encoding="utf-8"))

    def by_symbol(self, table):
        return {item["symbol"]: item for item in table["adapters"]}

    def test_fixture_compiles_six_adapter_records_and_preserves_boundaries(self):
        table = runtime_adapter_table.build_runtime_adapter_table(self.load_fixture())
        self.assertEqual(
            table["summary"],
            {
                "adapter_count": 6,
                "ready_count": 4,
                "pointer_validation_count": 2,
                "boundary_count": 2,
            },
        )
        self.assertEqual(
            [item["symbol"] for item in table["boundaries"]],
            ["_blocked_guest_stack", "_callback_boundary"],
        )

    def test_scalar_and_aggregate_guest_locations_are_normalized(self):
        table = runtime_adapter_table.build_runtime_adapter_table(self.load_fixture())
        items = self.by_symbol(table)
        add = items["_auto_add_u64"]
        self.assertEqual(add["arguments"][0]["capture"]["registers"], [0])
        self.assertEqual(add["arguments"][1]["capture"]["registers"], [1])
        self.assertEqual(add["result"]["commit"]["registers"], [0])

        hfa = items["_auto_hfa"]
        self.assertEqual(hfa["arguments"][0]["capture"]["bank"], "simd")
        self.assertEqual(hfa["arguments"][0]["capture"]["registers"], [0, 1])
        self.assertEqual(hfa["arguments"][0]["capture"]["element_width_bytes"], 4)
        self.assertEqual(
            [item["ffi_type"] for item in hfa["arguments"][0]["type"]["elements"]],
            ["ffi_type_float", "ffi_type_float"],
        )

    def test_pointer_records_require_explicit_runtime_validation(self):
        table = runtime_adapter_table.build_runtime_adapter_table(self.load_fixture())
        items = self.by_symbol(table)
        self.assertEqual(
            items["_auto_identity_pointer"]["execution_status"],
            "requires-pointer-validation",
        )
        self.assertTrue(items["_auto_identity_pointer"]["requires_pointer_validation"])
        self.assertEqual(
            items["_auto_indirect_store"]["arguments"][0]["capture"],
            {"kind": "result-pointer", "register": 8},
        )

    def test_nested_pointer_aggregate_remains_a_runtime_boundary(self):
        fixture = self.load_fixture()
        changed = copy.deepcopy(fixture["symbols"][0])
        changed["symbol"] = "_nested_pointer"
        nested = {
            "kind": "struct",
            "elements": [
                {
                    "kind": "builtin",
                    "ffi_type": "ffi_type_pointer",
                    "pointer_policy": "opaque-guest-address-requires-runtime-validation",
                },
                {"kind": "builtin", "ffi_type": "ffi_type_uint64"},
            ],
            "layout": "libffi-computed",
        }
        changed["arguments"][0]["ffi_type"] = nested
        changed["libffi"]["argument_types"][0] = copy.deepcopy(nested)
        fixture["symbols"].append(changed)
        table = runtime_adapter_table.build_runtime_adapter_table(fixture)
        boundaries = {item["symbol"]: item for item in table["boundaries"]}
        self.assertIn("_nested_pointer", boundaries)
        self.assertIn(
            "nested pointer",
            " ".join(boundaries["_nested_pointer"]["reasons"]).lower(),
        )

    def test_duplicate_symbols_and_descriptor_drift_fail_explicitly(self):
        fixture = self.load_fixture()
        fixture["symbols"].append(copy.deepcopy(fixture["symbols"][0]))
        with self.assertRaises(runtime_adapter_table.RuntimeAdapterTableError):
            runtime_adapter_table.build_runtime_adapter_table(fixture)

        fixture = self.load_fixture()
        fixture["symbols"][0]["libffi"]["argument_count"] = 99
        table = runtime_adapter_table.build_runtime_adapter_table(fixture)
        boundary = {item["symbol"]: item for item in table["boundaries"]}["_auto_add_u64"]
        self.assertIn("argument_count", " ".join(boundary["reasons"]))

    def test_cpp_fixture_is_deterministic_and_has_no_manual_drift(self):
        table = runtime_adapter_table.build_runtime_adapter_table(self.load_fixture())
        rendered = runtime_adapter_table.render_cpp_table(
            table, "makeGeneratedBridgeAdapterFixture"
        )
        self.assertEqual(rendered, GENERATED_CPP.read_text(encoding="utf-8"))
        self.assertEqual(
            rendered,
            runtime_adapter_table.render_cpp_table(
                runtime_adapter_table.build_runtime_adapter_table(self.load_fixture()),
                "makeGeneratedBridgeAdapterFixture",
            ),
        )

    def test_cli_writes_json_and_cpp_without_partial_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            json_out = Path(tmp) / "table.json"
            cpp_out = Path(tmp) / "table.inc"
            code = runtime_adapter_table.main(
                [
                    "--bridge-plan",
                    str(FIXTURE),
                    "--output",
                    str(json_out),
                    "--cpp-output",
                    str(cpp_out),
                    "--cpp-function-name",
                    "makeGeneratedBridgeAdapterFixture",
                ]
            )
            self.assertEqual(code, 0)
            self.assertEqual(
                json.loads(json_out.read_text(encoding="utf-8"))["kind"],
                "runtime-adapter-table",
            )
            self.assertEqual(
                cpp_out.read_text(encoding="utf-8"),
                GENERATED_CPP.read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    unittest.main()
