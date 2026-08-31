import unittest

import tbd_surface as ts


V3_COHERENT = """\
--- !tapi-tbd-v3
archs: [ arm64, arm64e ]
platform: ios
install-name: /System/Library/Frameworks/Test.framework/Test
current-version: 126.4.1
compatibility-version: 1
exports:
  - archs: [ arm64, arm64e ]
    symbols: [ _alpha, _beta ]
    objc-classes: [ OS_test_object ]
    re-exports: [ /usr/lib/libChild.dylib ]
"""

V4_COHERENT = """\
--- !tapi-tbd
tbd-version: 4
targets: [ arm64-ios, arm64e-ios ]
install-name: '/System/Library/Frameworks/Test.framework/Test'
current-version: 126.3.5
reexported-libraries:
  - targets: [ arm64-ios, arm64e-ios ]
    libraries: [ '/usr/lib/libChild.dylib' ]
exports:
  - targets: [ arm64-ios, arm64e-ios ]
    symbols: [ _alpha, _beta ]
"""


class MixedTapiFormatTests(unittest.TestCase):
    def test_coherent_v3_and_v4_representations_merge_without_losing_provenance(self):
        v3 = ts.parse_tbd_text(V3_COHERENT, "standalone/Test.tbd")
        v4 = ts.parse_tbd_text(V4_COHERENT, "Umbrella.tbd")

        manifest = ts.build_sdk_manifest(v3 + v4)

        self.assertEqual(manifest["summary"]["interface_count"], 1)
        interface = manifest["interfaces"][0]
        self.assertEqual(interface["format_version"], 4)
        self.assertEqual(interface["source_format_versions"], [3, 4])
        self.assertIsNone(interface["current_version"])
        self.assertEqual(
            interface["source_current_versions"],
            ["126.3.5", "126.4.1"],
        )
        self.assertEqual(interface["compatibility_version"], "1")
        self.assertEqual(
            interface["sources"],
            ["Umbrella.tbd", "standalone/Test.tbd"],
        )
        self.assertEqual(
            [(item["name"], item["kind"]) for item in interface["exports"]],
            [
                ("OS_test_object", "objc-class"),
                ("_alpha", "global"),
                ("_beta", "global"),
            ],
        )
        self.assertEqual(
            interface["reexports"],
            [
                {
                    "install_name": "/usr/lib/libChild.dylib",
                    "targets": ["arm64-ios", "arm64e-ios"],
                }
            ],
        )

    def test_mixed_formats_with_conflicting_shared_export_kind_fail_closed(self):
        v3 = ts.parse_tbd_text(V3_COHERENT, "standalone/Test.tbd")
        conflicting = ts.parse_tbd_text(
            V4_COHERENT.replace("_alpha, _beta", "_alpha, _gamma"),
            "Umbrella.tbd",
        )

        with self.assertRaisesRegex(
            ts.TbdParseError,
            r"conflicting export evidence on arm64-ios across TAPI format versions \[3, 4\] for kind 'global'",
        ):
            ts.build_sdk_manifest(v3 + conflicting)

    def test_mixed_formats_with_conflicting_reexports_fail_closed(self):
        v3 = ts.parse_tbd_text(V3_COHERENT, "standalone/Test.tbd")
        conflicting = ts.parse_tbd_text(
            V4_COHERENT.replace("/usr/lib/libChild.dylib", "/usr/lib/libOther.dylib"),
            "Umbrella.tbd",
        )

        with self.assertRaisesRegex(
            ts.TbdParseError,
            r"conflicting re-export evidence on arm64-ios across TAPI format versions \[3, 4\]",
        ):
            ts.build_sdk_manifest(v3 + conflicting)

    def test_reexport_evidence_present_in_only_one_format_is_preserved(self):
        v3 = ts.parse_tbd_text(V3_COHERENT, "standalone/Test.tbd")
        v4 = ts.parse_tbd_text(
            V4_COHERENT.replace(
                "reexported-libraries:\n  - targets: [ arm64-ios, arm64e-ios ]\n    libraries: [ '/usr/lib/libChild.dylib' ]\n",
                "",
            ),
            "Umbrella.tbd",
        )

        manifest = ts.build_sdk_manifest(v3 + v4)
        interface = manifest["interfaces"][0]
        self.assertEqual(
            interface["reexports"],
            [
                {
                    "install_name": "/usr/lib/libChild.dylib",
                    "targets": ["arm64-ios", "arm64e-ios"],
                }
            ],
        )


if __name__ == "__main__":
    unittest.main()
