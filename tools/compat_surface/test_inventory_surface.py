import json
import tempfile
import unittest
from pathlib import Path

import inventory_surface as inv


def macho_manifest(imports, image="libdispatch.dylib"):
    return {
        "schema_version": 1,
        "summary": {},
        "images": [
            {
                "image": image,
                "chained_fixups": True,
                "chained_import_format": 1,
                "dependencies": [],
                "imports": imports,
            }
        ],
    }


def imp(
    symbol,
    provider="/usr/lib/system/libsystem_kernel.dylib",
    *,
    ordinal=1,
    provider_kind="dependency",
    weak=False,
    addend=0,
):
    return {
        "symbol": symbol,
        "ordinal": ordinal,
        "provider": provider,
        "provider_kind": provider_kind,
        "weak": weak,
        "addend": addend,
    }


def tapi_manifest(interfaces, symbol_index):
    return {
        "schema_version": 1,
        "kind": "tapi-sdk-surface",
        "summary": {},
        "interfaces": interfaces,
        "symbol_index": symbol_index,
    }


def interface(name, *, reexports=(), targets=("arm64-ios",)):
    return {
        "install_name": name,
        "format_version": 4,
        "current_version": "1",
        "compatibility_version": "1",
        "targets": list(targets),
        "sources": ["usr/lib/example.tbd"],
        "exports": [],
        "reexports": [
            {"install_name": child, "targets": list(targets)}
            for child in reexports
        ],
    }


def export(symbol, provider, *, weak=False, targets=("arm64-ios",)):
    return {
        "name": symbol,
        "kind": "global",
        "weak": weak,
        "providers": [
            {"install_name": provider, "targets": list(targets)}
        ],
    }


def header_manifest(signatures):
    return {
        "schema_version": 1,
        "kind": "header-signature-surface",
        "target": "arm64-apple-ios16.0",
        "summary": {},
        "signatures": signatures,
    }


def signature(symbol, *, variadic=False, prototype=True):
    return {
        "symbol": symbol,
        "names": [symbol.lstrip("_")],
        "function_type_spellings": ["int (int)"],
        "calling_convention": "cdecl",
        "variadic": variadic,
        "prototype": prototype,
        "return_type": {"kind": "BuiltinType", "qual_type": "int"},
        "parameters": [
            {
                "index": 0,
                "names": ["value"],
                "spellings": ["int"],
                "type": {"kind": "BuiltinType", "qual_type": "int"},
            }
        ],
        "sources": [{"header": "usr/include/example.h", "line": 1, "column": 1}],
    }


class InventorySurfaceTests(unittest.TestCase):
    def test_direct_provider_and_signature_join(self):
        provider = "/usr/lib/system/libsystem_kernel.dylib"
        result = inv.build_inventory(
            macho_manifest([imp("_read", provider)]),
            tapi_manifest([interface(provider)], [export("_read", provider)]),
            header_manifest([signature("_read")]),
        )
        req = result["requirements"][0]
        self.assertTrue(req["header_signature_available"])
        self.assertEqual(req["provider_match"]["kind"], "direct")
        self.assertEqual(req["provider_match"]["path"], [provider])
        self.assertEqual(result["summary"]["typed_requirement_count"], 1)
        self.assertEqual(result["summary"]["direct_provider_match_count"], 1)
        self.assertEqual(result["symbols"][0]["signature"]["symbol"], "_read")

    def test_reexport_path_is_explicit_and_shortest(self):
        umbrella = "/usr/lib/libSystem.B.dylib"
        middle = "/usr/lib/system/libmiddle.dylib"
        provider = "/usr/lib/system/libsystem_kernel.dylib"
        result = inv.build_inventory(
            macho_manifest([imp("_read", umbrella)]),
            tapi_manifest(
                [
                    interface(umbrella, reexports=(middle, provider)),
                    interface(middle, reexports=(provider,)),
                    interface(provider),
                ],
                [export("_read", provider)],
            ),
            header_manifest([signature("_read")]),
        )
        match = result["requirements"][0]["provider_match"]
        self.assertEqual(match["kind"], "reexport")
        self.assertEqual(match["path"], [umbrella, provider])
        self.assertEqual(result["summary"]["reexport_provider_match_count"], 1)

    def test_provider_mismatch_is_reported_not_normalized(self):
        expected = "/usr/lib/system/libsystem_sim_kernel.dylib"
        actual = "/usr/lib/system/libsystem_kernel.dylib"
        result = inv.build_inventory(
            macho_manifest([imp("_read", expected)]),
            tapi_manifest([interface(actual)], [export("_read", actual)]),
            header_manifest([signature("_read")]),
        )
        req = result["requirements"][0]
        self.assertEqual(req["provider_match"]["kind"], "provider-mismatch")
        self.assertEqual(req["sdk_direct_exports"][0]["install_name"], actual)
        self.assertEqual(result["summary"]["provider_mismatch_count"], 1)

    def test_missing_sdk_symbol_and_missing_signature_remain_explicit(self):
        provider = "/usr/lib/system/libsystem_kernel.dylib"
        result = inv.build_inventory(
            macho_manifest([imp("_unknown", provider)]),
            tapi_manifest([interface(provider)], []),
            header_manifest([]),
        )
        req = result["requirements"][0]
        self.assertFalse(req["header_signature_available"])
        self.assertEqual(req["provider_match"]["kind"], "sdk-symbol-absent")
        self.assertEqual(result["summary"]["untyped_requirement_count"], 1)
        self.assertEqual(result["summary"]["sdk_symbol_absent_count"], 1)
        self.assertIsNone(result["symbols"][0]["signature"])

    def test_special_ordinal_is_not_forced_to_sdk_provider(self):
        provider = "/usr/lib/system/libsystem_kernel.dylib"
        result = inv.build_inventory(
            macho_manifest(
                [
                    imp(
                        "_read",
                        None,
                        ordinal=-2,
                        provider_kind="flat-lookup",
                        weak=True,
                    )
                ]
            ),
            tapi_manifest([interface(provider)], [export("_read", provider)]),
            header_manifest([signature("_read")]),
        )
        req = result["requirements"][0]
        self.assertEqual(req["provider_match"]["kind"], "special-ordinal")
        self.assertEqual(
            req["provider_match"]["special_provider_kind"], "flat-lookup"
        )
        self.assertEqual(req["provider_match"]["path"], [])
        self.assertEqual(result["summary"]["special_ordinal_count"], 1)
        self.assertEqual(result["summary"]["weak_import_count"], 1)

    def test_target_filter_does_not_mix_arm64e_only_evidence(self):
        provider = "/usr/lib/system/libsystem_kernel.dylib"
        result = inv.build_inventory(
            macho_manifest([imp("_read", provider)]),
            tapi_manifest(
                [interface(provider, targets=("arm64e-ios",))],
                [export("_read", provider, targets=("arm64e-ios",))],
            ),
            header_manifest([signature("_read")]),
        )
        self.assertEqual(
            result["requirements"][0]["provider_match"]["kind"],
            "sdk-symbol-absent",
        )
        self.assertEqual(result["targets"]["tapi"], "arm64-ios")

    def test_variadic_and_no_prototype_counts_are_preserved(self):
        provider = "/usr/lib/system/libsystem_c.dylib"
        result = inv.build_inventory(
            macho_manifest(
                [imp("_printf", provider), imp("_legacy", provider)]
            ),
            tapi_manifest(
                [interface(provider)],
                [export("_printf", provider), export("_legacy", provider)],
            ),
            header_manifest(
                [
                    signature("_printf", variadic=True),
                    signature("_legacy", prototype=False),
                ]
            ),
        )
        self.assertEqual(result["summary"]["variadic_typed_requirement_count"], 1)
        self.assertEqual(
            result["summary"]["no_prototype_typed_requirement_count"], 1
        )

    def test_manifest_is_deterministic_across_input_order(self):
        a = "/usr/lib/A.dylib"
        b = "/usr/lib/B.dylib"
        imports1 = [imp("_z", b), imp("_a", a)]
        imports2 = list(reversed(imports1))
        tapi1 = tapi_manifest(
            [interface(b), interface(a)],
            [export("_z", b), export("_a", a)],
        )
        tapi2 = tapi_manifest(
            list(reversed(tapi1["interfaces"])),
            list(reversed(tapi1["symbol_index"])),
        )
        headers1 = header_manifest([signature("_z"), signature("_a")])
        headers2 = header_manifest(list(reversed(headers1["signatures"])))
        one = inv.build_inventory(macho_manifest(imports1), tapi1, headers1)
        two = inv.build_inventory(macho_manifest(imports2), tapi2, headers2)
        self.assertEqual(
            json.dumps(one, separators=(",", ":")),
            json.dumps(two, separators=(",", ":")),
        )

    def test_duplicate_header_signature_fails_explicitly(self):
        provider = "/usr/lib/A.dylib"
        with self.assertRaisesRegex(inv.InventoryError, "duplicate header signature"):
            inv.build_inventory(
                macho_manifest([imp("_a", provider)]),
                tapi_manifest([interface(provider)], [export("_a", provider)]),
                header_manifest([signature("_a"), signature("_a")]),
            )

    def test_wrong_schema_or_kind_fails_explicitly(self):
        provider = "/usr/lib/A.dylib"
        bad_tapi = tapi_manifest([interface(provider)], [export("_a", provider)])
        bad_tapi["kind"] = "wrong"
        with self.assertRaisesRegex(inv.InventoryError, "TAPI kind"):
            inv.build_inventory(
                macho_manifest([imp("_a", provider)]),
                bad_tapi,
                header_manifest([signature("_a")]),
            )

    def test_cli_writes_inventory_without_embedding_manifest_paths(self):
        provider = "/usr/lib/A.dylib"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {
                "macho": root / "private-macho.json",
                "tapi": root / "private-tapi.json",
                "headers": root / "private-headers.json",
                "output": root / "inventory.json",
            }
            paths["macho"].write_text(
                json.dumps(macho_manifest([imp("_a", provider)])),
                encoding="utf-8",
            )
            paths["tapi"].write_text(
                json.dumps(
                    tapi_manifest(
                        [interface(provider)], [export("_a", provider)]
                    )
                ),
                encoding="utf-8",
            )
            paths["headers"].write_text(
                json.dumps(header_manifest([signature("_a")])),
                encoding="utf-8",
            )
            exit_code = inv.main(
                [
                    "--macho",
                    str(paths["macho"]),
                    "--tapi",
                    str(paths["tapi"]),
                    "--headers",
                    str(paths["headers"]),
                    "--output",
                    str(paths["output"]),
                ]
            )
            self.assertEqual(exit_code, 0)
            rendered = paths["output"].read_text(encoding="utf-8")
            self.assertNotIn(str(root), rendered)
            parsed = json.loads(rendered)
            self.assertEqual(parsed["kind"], "typed-compatibility-inventory")


if __name__ == "__main__":
    unittest.main()
