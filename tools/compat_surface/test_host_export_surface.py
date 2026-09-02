import pathlib
import unittest

import host_export_surface


ROOT = pathlib.Path(__file__).resolve().parents[2]
DARWIN_HOST_DEF = ROOT / "src" / "IpaSimulator" / "DarwinHostBridge.def"


class HostExportSurfaceTests(unittest.TestCase):
    def test_parses_alias_data_and_optional_attributes(self):
        manifest = host_export_surface.parse_def_text(
            """
            LIBRARY ExampleHost
            EXPORTS
                getuid=darwin_getuid
                global_value DATA
                ordinal_name=internal_name @7 NONAME PRIVATE
            """,
            source="fixture.def",
        )

        self.assertEqual(manifest["library"], "ExampleHost")
        self.assertEqual(manifest["library_filename"], "ExampleHost.dll")
        by_name = {item["name"]: item for item in manifest["exports"]}
        self.assertEqual(by_name["getuid"]["target"], "darwin_getuid")
        self.assertEqual(by_name["getuid"]["kind"], "function")
        self.assertEqual(by_name["global_value"]["kind"], "data")
        self.assertEqual(by_name["ordinal_name"]["ordinal"], 7)
        self.assertTrue(by_name["ordinal_name"]["noname"])
        self.assertTrue(by_name["ordinal_name"]["private"])

    def test_duplicate_export_fails_closed(self):
        with self.assertRaisesRegex(
            host_export_surface.HostExportSurfaceError,
            "duplicate export",
        ):
            host_export_surface.parse_def_text(
                "LIBRARY Host\nEXPORTS\nfoo\nfoo\n",
                source="duplicate.def",
            )

    def test_unknown_export_attribute_fails_closed(self):
        with self.assertRaisesRegex(
            host_export_surface.HostExportSurfaceError,
            "unsupported export attribute",
        ):
            host_export_surface.parse_def_text(
                "LIBRARY Host\nEXPORTS\nfoo MAGIC\n",
                source="bad.def",
            )

    def test_production_darwin_host_surface_keeps_data_non_callable(self):
        manifest = host_export_surface.parse_def_file(DARWIN_HOST_DEF)
        by_name = {item["name"]: item for item in manifest["exports"]}

        self.assertEqual(manifest["library_filename"], "IpaSimDarwinHost.dll")
        self.assertEqual(by_name["getuid"]["target"], "darwin_getuid")
        self.assertEqual(by_name["open$NOCANCEL"]["target"], "darwin_open")
        self.assertEqual(by_name["mach_task_self_"]["kind"], "data")
        self.assertEqual(by_name["vm_page_size"]["kind"], "data")
        self.assertEqual(by_name["NDR_record"]["kind"], "data")


if __name__ == "__main__":
    unittest.main()
