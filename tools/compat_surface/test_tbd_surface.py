import json
import tempfile
import unittest
from pathlib import Path

import tbd_surface as ts


LEGACY_V1 = """\
---
archs: [ armv7, arm64, x86_64 ]
platform: ios
install-name: /usr/lib/libExample.dylib
current-version: 1.2.3
compatibility-version: 1
exports:
  - archs: [ arm64, x86_64 ]
    re-exports: [ /usr/lib/libChild.dylib ]
    symbols: [ _alpha, _shared ]
    weak-def-symbols: [ _weak ]
...
"""

V3_MULTI = """\
--- !tapi-tbd-v3
archs: [ arm64, arm64e ]
platform: ios
install-name: /usr/lib/libV3A.dylib
current-version: 3.0
exports:
  - archs: [ arm64, arm64e ]
    symbols: [ _v3a ]
    objc-classes: [ V3Class ]
--- !tapi-tbd-v3
archs: [ armv7, arm64 ]
platform: ios
install-name: /usr/lib/libV3B.dylib
exports:
  - archs: [ armv7 ]
    symbols: [ _old_only ]
  - archs: [ arm64 ]
    symbols: [ _v3b ]
"""

V4_MULTI = """\
--- !tapi-tbd
tbd-version: 4
targets: [ arm64-ios, arm64e-ios, x86_64-ios-simulator ]
install-name: '/usr/lib/libRoot.dylib'
current-version: 4.2
compatibility-version: 1
reexported-libraries:
  - targets: [ arm64-ios, arm64e-ios ]
    libraries: [ '/usr/lib/libChild.dylib' ]
exports:
  - targets: [ arm64-ios, arm64e-ios ]
    symbols: [ _root, _shared ]
    weak-symbols: [ _weak_root ]
    objc-classes: [ RootClass ]
  - targets: [ x86_64-ios-simulator ]
    symbols: [ _sim_only ]
--- !tapi-tbd
tbd-version: 4
targets: [ arm64-ios ]
install-name: '/usr/lib/libChild.dylib'
exports:
  - targets: [ arm64-ios ]
    symbols: [ _child, _shared ]
    thread-local-symbols: [ _tls ]
"""


class TbdSurfaceTests(unittest.TestCase):
    def test_legacy_v1_normalizes_arm64_exports_and_reexports(self):
        interfaces = ts.parse_tbd_text(LEGACY_V1, "legacy.tbd")
        self.assertEqual(len(interfaces), 1)
        interface = interfaces[0]
        self.assertEqual(interface.format_version, 1)
        self.assertEqual(interface.targets, ("arm64-ios",))
        self.assertEqual(
            [item.name for item in interface.exports],
            ["_alpha", "_shared", "_weak"],
        )
        weak = next(item for item in interface.exports if item.name == "_weak")
        self.assertTrue(weak.weak)
        self.assertEqual(
            [item.install_name for item in interface.reexports],
            ["/usr/lib/libChild.dylib"],
        )

    def test_v3_unknown_yaml_tags_and_group_filtering(self):
        interfaces = ts.parse_tbd_text(V3_MULTI, "v3.tbd")
        self.assertEqual(
            [item.install_name for item in interfaces],
            ["/usr/lib/libV3A.dylib", "/usr/lib/libV3B.dylib"],
        )
        a = interfaces[0]
        self.assertEqual(a.format_version, 3)
        self.assertEqual(a.targets, ("arm64-ios", "arm64e-ios"))
        self.assertEqual(
            [(item.name, item.kind) for item in a.exports],
            [("V3Class", "objc-class"), ("_v3a", "global")],
        )
        b = interfaces[1]
        self.assertEqual([item.name for item in b.exports], ["_v3b"])

    def test_v4_multi_document_filters_device_targets(self):
        interfaces = ts.parse_tbd_text(V4_MULTI, "v4.tbd")
        root = interfaces[0]
        self.assertEqual(root.format_version, 4)
        self.assertNotIn("_sim_only", [item.name for item in root.exports])
        self.assertEqual(
            root.reexports[0].install_name, "/usr/lib/libChild.dylib"
        )
        root_class = next(item for item in root.exports if item.name == "RootClass")
        self.assertEqual(root_class.kind, "objc-class")
        self.assertEqual(root_class.targets, ("arm64-ios", "arm64e-ios"))

    def test_manifest_is_deterministic_and_indexes_direct_providers(self):
        interfaces = ts.parse_tbd_text(V4_MULTI, "System/Library/libRoot.tbd")
        manifest1 = ts.build_sdk_manifest(interfaces)
        manifest2 = ts.build_sdk_manifest(reversed(interfaces))
        self.assertEqual(
            json.dumps(manifest1, separators=(",", ":")),
            json.dumps(manifest2, separators=(",", ":")),
        )
        shared = next(
            item for item in manifest1["symbol_index"] if item["name"] == "_shared"
        )
        self.assertEqual(
            [provider["install_name"] for provider in shared["providers"]],
            ["/usr/lib/libChild.dylib", "/usr/lib/libRoot.dylib"],
        )
        self.assertEqual(manifest1["summary"]["interface_count"], 2)
        self.assertEqual(manifest1["summary"]["reexport_count"], 1)

    def test_conflicting_duplicate_install_names_fail_explicitly(self):
        a = ts.parse_tbd_text(
            """---
archs: [ arm64 ]
platform: ios
install-name: /usr/lib/libSame.dylib
current-version: 1
exports: []
""",
            "a.tbd",
        )
        b = ts.parse_tbd_text(
            """---
archs: [ arm64 ]
platform: ios
install-name: /usr/lib/libSame.dylib
current-version: 2
exports: []
""",
            "b.tbd",
        )
        with self.assertRaisesRegex(ts.TbdParseError, "conflicting current-version"):
            ts.build_sdk_manifest(a + b)

    def test_cli_sdk_root_keeps_paths_relative(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "usr/lib/libExample.tbd"
            target.parent.mkdir(parents=True)
            target.write_text(LEGACY_V1, encoding="utf-8")
            output = root / "sdk-surface.json"
            code = ts.main(
                [
                    "--sdk-root",
                    str(root),
                    "--output",
                    str(output),
                ]
            )
            self.assertEqual(code, 0)
            rendered = output.read_text(encoding="utf-8")
            manifest = json.loads(rendered)
            self.assertEqual(
                manifest["interfaces"][0]["sources"],
                ["usr/lib/libExample.tbd"],
            )
            self.assertNotIn(str(root), rendered)

    def test_non_ios_legacy_document_is_ignored(self):
        text = """---
archs: [ arm64 ]
platform: macosx
install-name: /usr/lib/libMacOnly.dylib
exports:
  - archs: [ arm64 ]
    symbols: [ _mac ]
"""
        self.assertEqual(ts.parse_tbd_text(text, "mac.tbd"), [])


if __name__ == "__main__":
    unittest.main()
