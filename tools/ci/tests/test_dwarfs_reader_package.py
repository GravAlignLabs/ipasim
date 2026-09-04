"""Contract tests using the real pinned package; mutations must fail closed."""

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    'reader_package', ROOT / 'tools/ci/prepare-dwarfs-reader.py')
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class ReaderPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ipasim-reader-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copytree(ROOT / 'deps/dwarfs-reader', self.root / 'deps/dwarfs-reader')
        for name in PACKAGE.SOURCE_INPUTS:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        self.path = self.root / PACKAGE.MANIFEST
        self.manifest = json.loads(self.path.read_text())
        self.archive = self.root / self.manifest['archive']['path']

    def save_manifest(self):
        self.path.write_text(json.dumps(self.manifest), encoding='utf-8')

    def rewrite_zip(self, entries):
        with zipfile.ZipFile(self.archive, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, content in entries:
                archive.writestr(name, content)
        data = self.archive.read_bytes()
        self.manifest['archive']['size'] = len(data)
        self.manifest['archive']['sha256'] = hashlib.sha256(data).hexdigest()
        self.save_manifest()

    def test_real_package(self):
        manifest, dll = PACKAGE.read_package(self.root)
        self.assertEqual(len(dll), 4329472)
        self.assertEqual(manifest['abi_version'], 1)

    def test_crlf_checkout_keeps_source_identity(self):
        for name in PACKAGE.SOURCE_INPUTS | set(self.manifest['notices']):
            path = self.root / name
            path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        PACKAGE.read_package(self.root)

    def test_changed_reader_requires_explicit_repin(self):
        source = self.root / 'src/IpaSimulator/DwarfsReaderBridge.cpp'
        source.write_bytes(source.read_bytes() + b'\n// modified\n')
        with self.assertRaisesRegex(ValueError, 'automatic rebuilding is disabled'):
            PACKAGE.read_package(self.root)

    def test_missing_source_fingerprint(self):
        self.manifest['source_files'].pop('include/ipasim/DwarfsReaderBridgeApi.h')
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'fingerprint set'):
            PACKAGE.read_package(self.root)

    def test_missing_archive(self):
        self.archive.unlink()
        with self.assertRaises(FileNotFoundError):
            PACKAGE.read_package(self.root)

    def test_corrupt_archive(self):
        data = bytearray(self.archive.read_bytes())
        data[70] ^= 1
        self.archive.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'archive SHA-256 mismatch'):
            PACKAGE.read_package(self.root)

    def test_wrong_archive_size(self):
        self.manifest['archive']['size'] += 1
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'archive size mismatch'):
            PACKAGE.read_package(self.root)

    def test_wrong_binary_hash(self):
        self.manifest['binary']['sha256'] = '0' * 64
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'DLL SHA-256 mismatch'):
            PACKAGE.read_package(self.root)

    def test_wrong_binary_size(self):
        self.manifest['binary']['size'] += 1
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'member size or type'):
            PACKAGE.read_package(self.root)

    def test_missing_notices(self):
        self.manifest['notices'] = {}
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'no license notices'):
            PACKAGE.read_package(self.root)

    def test_changed_notice(self):
        path = self.root / next(iter(self.manifest['notices']))
        path.write_bytes(b'changed license')
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            PACKAGE.read_package(self.root)

    def test_metadata_versions(self):
        for field in ('schema_version', 'abi_version', 'platform'):
            with self.subTest(field=field):
                old = self.manifest[field]
                self.manifest[field] = 'unsupported'
                self.save_manifest()
                with self.assertRaisesRegex(ValueError, 'unsupported reader package'):
                    PACKAGE.read_package(self.root)
                self.manifest[field] = old

    def test_manifest_path_traversal(self):
        for path in ('../reader.zip', '/tmp/reader.zip', 'C:/reader.zip', '..\\reader.zip'):
            with self.subTest(path=path):
                self.manifest['archive']['path'] = path
                self.save_manifest()
                with self.assertRaisesRegex(ValueError, 'unsafe package path'):
                    PACKAGE.read_package(self.root)

    def test_archive_members(self):
        for entries in ([('../escape.dll', b'x')], [('other.dll', b'x')],
                        [('IpaSimDwarfsReader.dll', b'x')] * 2, []):
            with self.subTest(entries=entries):
                self.rewrite_zip(entries)
                with self.assertRaisesRegex(ValueError, 'must contain exactly'):
                    PACKAGE.read_package(self.root)

    def test_wrong_machine_even_with_matching_checksums(self):
        _, dll = PACKAGE.read_package(self.root)
        dll = bytearray(dll)
        pe = struct.unpack_from('<I', dll, 0x3c)[0]
        struct.pack_into('<H', dll, pe + 4, 0x14c)  # Deliberately corrupt to x86.
        self.manifest['binary']['sha256'] = hashlib.sha256(dll).hexdigest()
        self.rewrite_zip([('IpaSimDwarfsReader.dll', dll)])
        with self.assertRaisesRegex(ValueError, 'Windows x64'):
            PACKAGE.read_package(self.root)

    def test_workflow_has_no_reader_cache_or_build_fallback(self):
        workflow = (ROOT / '.github/workflows/runtime-root-dwarfs-reader.yml').read_text()
        reader = workflow.split('\n  reader-smoke:', 1)[1].split('\n  resolve-core-tester:', 1)[0]
        self.assertNotIn('actions/cache', reader)
        self.assertNotIn('build-dwarfs-reader.ps1', reader)
        self.assertIn('test-prebuilt-dwarfs-reader.ps1', reader)
        self.assertIn('Fail after reader diagnostic', reader)


if __name__ == '__main__':
    unittest.main()
