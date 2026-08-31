import json
import tempfile
import unittest
from pathlib import Path

import sdk_catalog as catalog


def tapi_manifest(interfaces, symbol_index):
    return {
        "schema_version": 1,
        "kind": "tapi-sdk-surface",
        "summary": {},
        "interfaces": interfaces,
        "symbol_index": symbol_index,
    }


def interface(name, targets=("arm64-ios",)):
    return {
        "install_name": name,
        "format_version": 4,
        "current_version": "1",
        "compatibility_version": "1",
        "targets": list(targets),
        "sources": ["usr/lib/example.tbd"],
        "exports": [],
        "reexports": [],
    }


def symbol_entry(name, kind, providers, *, weak=False):
    return {
        "name": name,
        "kind": kind,
        "weak": weak,
        "providers": [
            {
                "install_name": provider,
                "targets": list(targets),
            }
            for provider, targets in providers
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


def signature(symbol, *, header="usr/include/example.h"):
    return {
        "symbol": symbol,
        "names": [symbol.lstrip("_")],
        "function_type_spellings": ["int (void)"],
        "calling_convention": "cdecl",
        "variadic": False,
        "prototype": True,
        "return_type": {"kind": "builtin", "name": "int"},
        "parameters": [],
        "sources": [{"header": header, "line": 1, "column": 1}],
    }


class SdkCatalogTests(unittest.TestCase):
    def test_catalog_is_not_gated_on_macho_imports(self):
        provider = "/usr/lib/system/libsystem_c.dylib"
        result = catalog.build_sdk_catalog(
            tapi_manifest(
                [interface(provider)],
                [
                    symbol_entry(
                        "_alpha",
                        "global",
                        [(provider, ("arm64-ios",))],
                    ),
                    symbol_entry(
                        "_beta",
                        "global",
                        [(provider, ("arm64-ios",))],
                    ),
                ],
            ),
            header_manifest([signature("_alpha"), signature("_beta")]),
        )
        self.assertEqual(result["kind"], "typed-sdk-catalog")
        self.assertEqual(
            [item["symbol"] for item in result["symbols"]],
            ["_alpha", "_beta"],
        )
        self.assertEqual(result["summary"]["typed_global_symbol_count"], 2)
        self.assertTrue(all(item["callable_c_candidate"] for item in result["symbols"]))

    def test_non_c_metadata_is_preserved_but_never_promoted_to_callable_c(self):
        provider = "/usr/lib/libExample.dylib"
        result = catalog.build_sdk_catalog(
            tapi_manifest(
                [interface(provider)],
                [
                    symbol_entry(
                        "RootClass",
                        "objc-class",
                        [(provider, ("arm64-ios",))],
                    ),
                    symbol_entry(
                        "_tls_value",
                        "thread-local",
                        [(provider, ("arm64-ios",))],
                    ),
                ],
            ),
            # Deliberately provide a same-spelled header signature. The catalog
            # may retain that evidence, but non-global TAPI metadata must not
            # become a generated C-call candidate.
            header_manifest([signature("RootClass")]),
        )
        items = {item["symbol"]: item for item in result["symbols"]}
        self.assertEqual(items["RootClass"]["classification"], "objc-metadata")
        self.assertIsNotNone(items["RootClass"]["signature"])
        self.assertFalse(items["RootClass"]["callable_c_candidate"])
        self.assertEqual(items["_tls_value"]["classification"], "thread-local-data")
        self.assertFalse(items["_tls_value"]["callable_c_candidate"])
        self.assertEqual(result["summary"]["objc_symbol_count"], 1)
        self.assertEqual(result["summary"]["thread_local_symbol_count"], 1)

    def test_provider_weakness_and_multi_provider_evidence_remain_explicit(self):
        a = "/usr/lib/A.dylib"
        b = "/usr/lib/B.dylib"
        result = catalog.build_sdk_catalog(
            tapi_manifest(
                [interface(a), interface(b)],
                [
                    symbol_entry(
                        "_shared",
                        "global",
                        [(a, ("arm64-ios", "arm64e-ios"))],
                        weak=True,
                    ),
                    symbol_entry(
                        "_shared",
                        "global",
                        [(b, ("arm64-ios",))],
                        weak=False,
                    ),
                ],
            ),
            header_manifest([signature("_shared")]),
        )
        item = result["symbols"][0]
        self.assertTrue(item["weak_export"])
        self.assertTrue(item["strong_export"])
        self.assertEqual(
            [fact["install_name"] for fact in item["sdk_direct_exports"]],
            [a, b],
        )
        self.assertEqual(
            item["sdk_direct_exports"][0]["targets"],
            ["arm64-ios", "arm64e-ios"],
        )
        self.assertEqual(result["summary"]["multi_provider_symbol_count"], 1)

    def test_target_filter_excludes_arm64e_only_symbols(self):
        provider = "/usr/lib/A.dylib"
        result = catalog.build_sdk_catalog(
            tapi_manifest(
                [interface(provider, targets=("arm64-ios", "arm64e-ios"))],
                [
                    symbol_entry(
                        "_arm64",
                        "global",
                        [(provider, ("arm64-ios",))],
                    ),
                    symbol_entry(
                        "_arm64e_only",
                        "global",
                        [(provider, ("arm64e-ios",))],
                    ),
                ],
            ),
            header_manifest([signature("_arm64"), signature("_arm64e_only")]),
        )
        self.assertEqual([item["symbol"] for item in result["symbols"]], ["_arm64"])
        self.assertEqual(result["targets"]["tapi"], "arm64-ios")
        self.assertEqual(result["orphan_header_signatures"], ["_arm64e_only"])

    def test_untyped_global_remains_unknown_instead_of_becoming_a_function(self):
        provider = "/usr/lib/A.dylib"
        result = catalog.build_sdk_catalog(
            tapi_manifest(
                [interface(provider)],
                [
                    symbol_entry(
                        "_data_or_hidden_function",
                        "global",
                        [(provider, ("arm64-ios",))],
                    )
                ],
            ),
            header_manifest([]),
        )
        item = result["symbols"][0]
        self.assertEqual(item["classification"], "untyped-global")
        self.assertFalse(item["callable_c_candidate"])
        self.assertIsNone(item["signature"])
        self.assertEqual(result["summary"]["untyped_global_symbol_count"], 1)

    def test_abi_projection_contains_all_typed_globals_without_runtime_requirements(self):
        provider = "/usr/lib/A.dylib"
        sdk = catalog.build_sdk_catalog(
            tapi_manifest(
                [interface(provider)],
                [
                    symbol_entry("_alpha", "global", [(provider, ("arm64-ios",))]),
                    symbol_entry("_unknown", "global", [(provider, ("arm64-ios",))]),
                    symbol_entry("RootClass", "objc-class", [(provider, ("arm64-ios",))]),
                ],
            ),
            header_manifest([signature("_alpha"), signature("RootClass")]),
        )
        projected = catalog.build_abi_inventory(sdk)
        self.assertEqual(projected["kind"], "typed-compatibility-inventory")
        self.assertEqual(projected["scope"], "sdk-wide-mechanical-projection")
        self.assertEqual(projected["requirements"], [])
        self.assertEqual(projected["summary"]["requirement_count"], 0)
        self.assertEqual(
            [item["symbol"] for item in projected["symbols"]],
            ["_alpha"],
        )
        self.assertEqual(projected["symbols"][0]["required_by"], [])
        self.assertEqual(projected["symbols"][0]["requirement_count"], 0)

    def test_abi_projection_fails_if_callable_marker_loses_signature(self):
        broken = {
            "schema_version": 1,
            "kind": "typed-sdk-catalog",
            "targets": {
                "clang": "arm64-apple-ios16.0",
                "tapi": "arm64-ios",
            },
            "summary": {},
            "symbols": [
                {
                    "symbol": "_broken",
                    "callable_c_candidate": True,
                    "sdk_direct_exports": [
                        {
                            "install_name": "/usr/lib/A.dylib",
                            "kind": "global",
                            "weak": False,
                            "targets": ["arm64-ios"],
                        }
                    ],
                    "signature": None,
                }
            ],
            "orphan_header_signatures": [],
        }
        with self.assertRaisesRegex(catalog.SdkCatalogError, "callable without a Clang signature"):
            catalog.build_abi_inventory(broken)

    def test_catalog_is_deterministic_across_input_order(self):
        a = "/usr/lib/A.dylib"
        b = "/usr/lib/B.dylib"
        entries = [
            symbol_entry("_z", "global", [(b, ("arm64-ios",))]),
            symbol_entry("_a", "global", [(a, ("arm64-ios",))]),
        ]
        signatures = [signature("_z"), signature("_a")]
        one = catalog.build_sdk_catalog(
            tapi_manifest([interface(b), interface(a)], entries),
            header_manifest(signatures),
        )
        two = catalog.build_sdk_catalog(
            tapi_manifest([interface(a), interface(b)], list(reversed(entries))),
            header_manifest(list(reversed(signatures))),
        )
        self.assertEqual(
            json.dumps(one, separators=(",", ":")),
            json.dumps(two, separators=(",", ":")),
        )

    def test_cli_requires_no_macho_and_does_not_embed_manifest_paths(self):
        provider = "/usr/lib/A.dylib"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tapi_path = root / "private-sdk-surface.json"
            header_path = root / "private-header-surface.json"
            output = root / "catalog.json"
            abi_output = root / "abi-inventory.json"
            tapi_path.write_text(
                json.dumps(
                    tapi_manifest(
                        [interface(provider)],
                        [
                            symbol_entry(
                                "_alpha",
                                "global",
                                [(provider, ("arm64-ios",))],
                            )
                        ],
                    )
                ),
                encoding="utf-8",
            )
            header_path.write_text(
                json.dumps(header_manifest([signature("_alpha")])),
                encoding="utf-8",
            )
            code = catalog.main(
                [
                    "--tapi",
                    str(tapi_path),
                    "--headers",
                    str(header_path),
                    "--output",
                    str(output),
                    "--abi-inventory-output",
                    str(abi_output),
                ]
            )
            self.assertEqual(code, 0)
            rendered = output.read_text(encoding="utf-8")
            projected = abi_output.read_text(encoding="utf-8")
            self.assertNotIn(str(root), rendered)
            self.assertNotIn(str(root), projected)
            parsed = json.loads(rendered)
            parsed_projection = json.loads(projected)
            self.assertEqual(parsed["kind"], "typed-sdk-catalog")
            self.assertEqual(parsed["summary"]["symbol_count"], 1)
            self.assertEqual(
                parsed_projection["scope"],
                "sdk-wide-mechanical-projection",
            )
            self.assertEqual(parsed_projection["requirements"], [])

    def test_duplicate_header_signature_fails_closed(self):
        provider = "/usr/lib/A.dylib"
        with self.assertRaisesRegex(catalog.SdkCatalogError, "duplicate header signature"):
            catalog.build_sdk_catalog(
                tapi_manifest(
                    [interface(provider)],
                    [symbol_entry("_alpha", "global", [(provider, ("arm64-ios",))])],
                ),
                header_manifest([signature("_alpha"), signature("_alpha")]),
            )


if __name__ == "__main__":
    unittest.main()
