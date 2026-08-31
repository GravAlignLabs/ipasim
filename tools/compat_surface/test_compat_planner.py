import json
import unittest

import compat_planner as planner


PROVIDER = "/usr/lib/system/libsystem_c.dylib"


def signature(symbol):
    return {
        "symbol": symbol,
        "names": [symbol.lstrip("_")],
        "function_type_spellings": ["int (void)"],
        "calling_convention": "cdecl",
        "variadic": False,
        "prototype": True,
        "return_type": {"kind": "builtin", "name": "int"},
        "parameters": [],
        "sources": [{"header": "usr/include/example.h", "line": 1, "column": 1}],
    }


def catalog_manifest():
    return {
        "schema_version": 1,
        "kind": "typed-sdk-catalog",
        "targets": {
            "clang": "arm64-apple-ios16.0",
            "tapi": "arm64-ios",
        },
        "summary": {},
        "symbols": [
            {
                "symbol": "_alpha",
                "sdk_kinds": ["global"],
                "weak_export": False,
                "strong_export": True,
                "classification": "typed-c-function",
                "callable_c_candidate": True,
                "sdk_direct_exports": [
                    {
                        "install_name": PROVIDER,
                        "kind": "global",
                        "weak": False,
                        "targets": ["arm64-ios"],
                    }
                ],
                "signature": signature("_alpha"),
            },
            {
                "symbol": "_callback",
                "sdk_kinds": ["global"],
                "weak_export": False,
                "strong_export": True,
                "classification": "typed-c-function",
                "callable_c_candidate": True,
                "sdk_direct_exports": [
                    {
                        "install_name": PROVIDER,
                        "kind": "global",
                        "weak": False,
                        "targets": ["arm64-ios"],
                    }
                ],
                "signature": signature("_callback"),
            },
            {
                "symbol": "_unknown_global",
                "sdk_kinds": ["global"],
                "weak_export": False,
                "strong_export": True,
                "classification": "untyped-global",
                "callable_c_candidate": False,
                "sdk_direct_exports": [
                    {
                        "install_name": PROVIDER,
                        "kind": "global",
                        "weak": False,
                        "targets": ["arm64-ios"],
                    }
                ],
                "signature": None,
            },
            {
                "symbol": "RootClass",
                "sdk_kinds": ["objc-class"],
                "weak_export": False,
                "strong_export": True,
                "classification": "objc-metadata",
                "callable_c_candidate": False,
                "sdk_direct_exports": [
                    {
                        "install_name": "/System/Library/Frameworks/Example.framework/Example",
                        "kind": "objc-class",
                        "weak": False,
                        "targets": ["arm64-ios"],
                    }
                ],
                "signature": None,
            },
        ],
        "orphan_header_signatures": [],
    }


def abi_manifest():
    return {
        "schema_version": 1,
        "kind": "aapcs64-abi-surface",
        "target": "arm64-apple-ios16.0",
        "summary": {},
        "symbols": [
            {
                "symbol": "_alpha",
                "bridge_status": "generated-bridge-candidate",
                "bridge_reasons": [],
            },
            {
                "symbol": "_callback",
                "bridge_status": "callback-runtime",
                "bridge_reasons": [
                    "function/block pointer requires callback trampoline policy"
                ],
            },
        ],
    }


def semantic_manifest():
    return {
        "schema_version": 1,
        "kind": "semantic-provider-inventory",
        "providers": [
            {
                "guest_symbol": "_alpha",
                "status": "approved",
                "host_export": "alpha",
                "provider_module": "ipasimdarwinhost.dll",
                "adapter_symbol": "_alpha",
                "semantic_owner": "Fixture.alpha",
                "live_profile": "NoArgumentsSInt32ToX0",
                "evidence": "Synthetic approved provider used to prove planner classification.",
            },
            {
                "guest_symbol": "_callback",
                "status": "complex",
                "evidence": "Requires a host-to-guest callback trampoline policy.",
            },
        ],
    }


class CompatibilityPlannerTests(unittest.TestCase):
    @staticmethod
    def by_symbol(plan):
        return {item["symbol"]: item for item in plan["symbols"]}

    def test_planner_combines_sdk_abi_and_semantic_status_in_bulk(self):
        result = planner.build_plan(
            catalog_manifest(),
            semantic_manifest(),
            abi_manifest=abi_manifest(),
        )
        items = self.by_symbol(result)

        self.assertEqual(
            items["_alpha"]["mechanical_status"],
            "generated-bridge-candidate",
        )
        self.assertEqual(items["_alpha"]["semantic_status"], "approved")
        self.assertEqual(
            items["_alpha"]["route_status"],
            "approved-mechanical-route-ready",
        )

        self.assertEqual(items["_callback"]["mechanical_status"], "callback-runtime")
        self.assertEqual(items["_callback"]["semantic_status"], "complex")
        self.assertEqual(items["_callback"]["route_status"], "not-approved")

        self.assertEqual(items["_unknown_global"]["mechanical_status"], "not-callable-c")
        self.assertEqual(items["_unknown_global"]["semantic_status"], "unclassified")
        self.assertEqual(items["RootClass"]["mechanical_status"], "not-callable-c")

        self.assertEqual(result["summary"]["symbol_count"], 4)
        self.assertEqual(result["summary"]["typed_c_candidate_count"], 2)
        self.assertEqual(
            result["summary"]["route_status_counts"]["approved-mechanical-route-ready"],
            1,
        )

    def test_absent_aapcs64_surface_does_not_hide_typed_sdk_candidates(self):
        result = planner.build_plan(catalog_manifest(), semantic_manifest())
        items = self.by_symbol(result)
        self.assertEqual(
            items["_alpha"]["mechanical_status"],
            "typed-awaiting-aapcs64",
        )
        self.assertEqual(
            items["_alpha"]["route_status"],
            "approved-awaiting-aapcs64",
        )
        self.assertEqual(
            items["_callback"]["mechanical_status"],
            "typed-awaiting-aapcs64",
        )

    def test_provider_summary_exposes_high_leverage_work_categories(self):
        result = planner.build_plan(
            catalog_manifest(),
            semantic_manifest(),
            abi_manifest=abi_manifest(),
        )
        providers = {item["install_name"]: item for item in result["provider_summary"]}
        libc = providers[PROVIDER]
        self.assertEqual(libc["symbol_count"], 3)
        self.assertEqual(libc["typed_c_candidate_count"], 2)
        self.assertEqual(libc["generated_bridge_candidate_count"], 1)
        self.assertEqual(libc["callback_runtime_count"], 1)
        self.assertEqual(libc["semantic_approved_count"], 1)
        self.assertEqual(libc["semantic_complex_count"], 1)
        self.assertEqual(libc["semantic_unclassified_count"], 1)
        self.assertEqual(libc["approved_mechanical_route_ready_count"], 1)

    def test_unmentioned_semantics_are_unclassified_not_claimed_missing(self):
        result = planner.build_plan(
            catalog_manifest(),
            semantic_manifest(),
            abi_manifest=abi_manifest(),
        )
        items = self.by_symbol(result)
        self.assertEqual(items["_unknown_global"]["semantic_status"], "unclassified")
        self.assertNotEqual(items["_unknown_global"]["semantic_status"], "missing")

    def test_orphan_semantic_and_abi_rows_remain_visible(self):
        semantics = semantic_manifest()
        semantics["providers"].append(
            {
                "guest_symbol": "_outside_catalog",
                "status": "unsupported",
                "evidence": "Synthetic record outside this targeted SDK catalog.",
            }
        )
        abi = abi_manifest()
        abi["symbols"].append(
            {
                "symbol": "_abi_outside_catalog",
                "bridge_status": "generated-bridge-candidate",
                "bridge_reasons": [],
            }
        )
        result = planner.build_plan(catalog_manifest(), semantics, abi_manifest=abi)
        self.assertEqual(result["orphan_semantic_providers"], ["_outside_catalog"])
        self.assertEqual(result["orphan_aapcs64_symbols"], ["_abi_outside_catalog"])

    def test_target_mismatch_fails_closed(self):
        abi = abi_manifest()
        abi["target"] = "arm64-apple-ios17.0"
        with self.assertRaisesRegex(planner.CompatibilityPlannerError, "targets differ"):
            planner.build_plan(catalog_manifest(), semantic_manifest(), abi_manifest=abi)

    def test_plan_is_deterministic(self):
        one = planner.build_plan(
            catalog_manifest(),
            semantic_manifest(),
            abi_manifest=abi_manifest(),
        )
        reordered_catalog = catalog_manifest()
        reordered_catalog["symbols"] = list(reversed(reordered_catalog["symbols"]))
        reordered_semantics = semantic_manifest()
        reordered_semantics["providers"] = list(reversed(reordered_semantics["providers"]))
        reordered_abi = abi_manifest()
        reordered_abi["symbols"] = list(reversed(reordered_abi["symbols"]))
        two = planner.build_plan(
            reordered_catalog,
            reordered_semantics,
            abi_manifest=reordered_abi,
        )
        self.assertEqual(
            json.dumps(one, separators=(",", ":")),
            json.dumps(two, separators=(",", ":")),
        )


if __name__ == "__main__":
    unittest.main()
