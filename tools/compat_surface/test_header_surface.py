import json
import tempfile
import unittest
from pathlib import Path

import header_surface as hs


class HeaderSurfaceTests(unittest.TestCase):
    def write(self, root, name, text):
        path = Path(root) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def analyze(self, path, display=None, sdk_root=None):
        sigs, stats = hs.analyze_header(
            path, display, sdk_root=sdk_root
        )
        return hs.build_manifest(
            sigs,
            target=hs.DEFAULT_TARGET,
            headers=[display or path.name],
            stats=[stats],
        )

    def test_typedef_is_canonicalized_through_clang_type_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(
                directory,
                "api.h",
                "typedef unsigned long size_t;\n"
                "extern int foo(const char *s, size_t n);\n",
            )
            manifest = self.analyze(path)
            sig = manifest["signatures"][0]
            self.assertEqual(sig["symbol"], "_foo")
            self.assertEqual(
                sig["return_type"],
                {"kind": "builtin", "name": "int"},
            )
            self.assertEqual(
                sig["parameters"][1]["spellings"], ["size_t"]
            )
            self.assertEqual(
                sig["parameters"][1]["type"],
                {"kind": "builtin", "name": "unsigned long"},
            )

    def test_function_pointer_return_and_variadic_are_structural(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(
                directory,
                "callbacks.h",
                "typedef int (*callback_t)(double);\n"
                "extern callback_t get_cb(void);\n"
                "extern void emit(const char *fmt, ...);\n",
            )
            manifest = self.analyze(path)
            by_symbol = {
                item["symbol"]: item for item in manifest["signatures"]
            }
            ret = by_symbol["_get_cb"]["return_type"]
            self.assertEqual(ret["kind"], "pointer")
            self.assertEqual(ret["pointee"]["kind"], "function")
            self.assertEqual(
                ret["pointee"]["return"],
                {"kind": "builtin", "name": "int"},
            )
            self.assertTrue(by_symbol["_emit"]["variadic"])
            self.assertEqual(
                manifest["summary"]["variadic_symbol_count"], 1
            )

    def test_record_return_and_no_prototype_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(
                directory,
                "records.h",
                "typedef struct Pair { int a; double b; } Pair;\n"
                "extern Pair make_pair(int a, double b);\n"
                "extern int legacy();\n",
            )
            manifest = self.analyze(path)
            by_symbol = {
                item["symbol"]: item for item in manifest["signatures"]
            }
            self.assertEqual(
                by_symbol["_make_pair"]["return_type"],
                {"kind": "record", "name": "struct Pair"},
            )
            self.assertFalse(by_symbol["_legacy"]["prototype"])
            self.assertEqual(
                manifest["summary"]["no_prototype_symbol_count"], 1
            )

    def test_static_and_included_functions_are_not_claimed_as_header_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write(
                directory,
                "dep.h",
                "extern int dependency(int);\n",
            )
            path = self.write(
                directory,
                "api.h",
                '#include "dep.h"\n'
                "static inline int helper(int x) { return x; }\n"
                "extern int public_api(int x);\n",
            )
            manifest = self.analyze(path)
            self.assertEqual(
                [item["symbol"] for item in manifest["signatures"]],
                ["_public_api"],
            )
            self.assertEqual(
                manifest["summary"]["skipped_static_declaration_count"],
                1,
            )

    def test_coherent_duplicates_merge_and_conflicts_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            a = self.write(
                directory, "a.h", "extern int foo(int value);\n"
            )
            b = self.write(
                directory, "b.h", "extern int foo(int other);\n"
            )
            sa, sta = hs.analyze_header(a)
            sb, stb = hs.analyze_header(b)
            manifest = hs.build_manifest(
                sa + sb,
                target=hs.DEFAULT_TARGET,
                headers=["a.h", "b.h"],
                stats=[sta, stb],
            )
            sig = manifest["signatures"][0]
            self.assertEqual(
                [source["header"] for source in sig["sources"]],
                ["a.h", "b.h"],
            )
            self.assertEqual(
                sig["parameters"][0]["names"], ["other", "value"]
            )

            c = self.write(
                directory, "c.h", "extern long foo(long value);\n"
            )
            sc, stc = hs.analyze_header(c)
            with self.assertRaisesRegex(
                hs.HeaderParseError, "conflicting header signatures"
            ):
                hs.build_manifest(
                    sa + sc,
                    target=hs.DEFAULT_TARGET,
                    headers=["a.h", "c.h"],
                    stats=[sta, stc],
                )

    def test_sdk_root_paths_stay_relative_and_output_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "iPhoneOS16.5.sdk"
            path = self.write(
                root,
                "usr/include/demo.h",
                "extern int zebra(int);\n"
                "extern int alpha(int);\n",
            )
            sigs, stats = hs.analyze_header(
                path, "usr/include/demo.h", sdk_root=root
            )
            one = hs.build_manifest(
                sigs,
                target=hs.DEFAULT_TARGET,
                headers=["usr/include/demo.h"],
                stats=[stats],
            )
            two = hs.build_manifest(
                reversed(sigs),
                target=hs.DEFAULT_TARGET,
                headers=["usr/include/demo.h"],
                stats=[stats],
            )
            self.assertEqual(
                json.dumps(one, sort_keys=True),
                json.dumps(two, sort_keys=True),
            )
            rendered = json.dumps(one)
            self.assertNotIn(str(root), rendered)
            self.assertEqual(
                [item["symbol"] for item in one["signatures"]],
                ["_alpha", "_zebra"],
            )

    def test_cli_reports_clang_parse_failure_without_writing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(
                directory, "broken.h", "extern int broken( ;\n"
            )
            output = Path(directory) / "manifest.json"
            code = hs.main(
                [str(path), "--output", str(output)]
            )
            self.assertEqual(code, 1)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
