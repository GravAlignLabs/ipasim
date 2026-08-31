import shutil
import tempfile
import unittest
from pathlib import Path

import abi_surface
import sdk_catalog


def _interface(name):
    return {
        "install_name": name,
        "format_version": 4,
        "current_version": "1",
        "compatibility_version": "1",
        "targets": ["arm64-ios"],
        "sources": ["usr/lib/example.tbd"],
        "exports": [],
        "reexports": [],
    }


def _symbol(name, kind, provider):
    return {
        "name": name,
        "kind": kind,
        "weak": False,
        "providers": [
            {
                "install_name": provider,
                "targets": ["arm64-ios"],
            }
        ],
    }


def _signature(symbol, name, header, parameters):
    int_type = {"kind": "builtin", "name": "int"}
    return {
        "symbol": symbol,
        "names": [name],
        "function_type_spellings": [],
        "calling_convention": "cdecl",
        "variadic": False,
        "prototype": True,
        "return_type": int_type,
        "parameters": [
            {
                "index": index,
                "names": [],
                "spellings": ["int"],
                "type": int_type,
            }
            for index in range(parameters)
        ],
        "sources": [
            {
                "header": header,
                "line": 1,
                "column": 1,
            }
        ],
    }


class SdkCatalogAbiIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("clang") is None:
            raise unittest.SkipTest("clang is required")

    def test_sdk_wide_projection_runs_through_real_aapcs64_lowering(self):
        provider = "/usr/lib/system/libsystem_example.dylib"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "sdk_fixture.h"
            header.write_text(
                "int alpha(void);\n"
                "int beta(int value);\n",
                encoding="utf-8",
            )

            tapi = {
                "schema_version": 1,
                "kind": "tapi-sdk-surface",
                "summary": {},
                "interfaces": [_interface(provider)],
                "symbol_index": [
                    _symbol("_alpha", "global", provider),
                    _symbol("_beta", "global", provider),
                    _symbol("ExampleClass", "objc-class", provider),
                ],
            }
            headers = {
                "schema_version": 1,
                "kind": "header-signature-surface",
                "target": "arm64-apple-ios16.0",
                "summary": {},
                "signatures": [
                    _signature("_alpha", "alpha", header.name, 0),
                    _signature("_beta", "beta", header.name, 1),
                ],
            }

            catalog = sdk_catalog.build_sdk_catalog(tapi, headers)
            inventory = sdk_catalog.build_abi_inventory(catalog)
            guest_abi = abi_surface.build_abi_manifest(
                inventory,
                header_root=root,
            )

        self.assertEqual(inventory["requirements"], [])
        self.assertEqual(inventory["scope"], "sdk-wide-mechanical-projection")
        self.assertEqual(
            [item["symbol"] for item in guest_abi["symbols"]],
            ["_alpha", "_beta"],
        )
        by_symbol = {item["symbol"]: item for item in guest_abi["symbols"]}
        self.assertEqual(
            by_symbol["_alpha"]["bridge_status"],
            "generated-bridge-candidate",
        )
        self.assertEqual(
            by_symbol["_beta"]["parameters"][0]["location"]["registers"],
            ["x0"],
        )
        self.assertNotIn(
            "ExampleClass",
            {item["symbol"] for item in guest_abi["symbols"]},
        )


if __name__ == "__main__":
    unittest.main()
