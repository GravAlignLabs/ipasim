#!/usr/bin/env python3
"""Verify and unpack the pinned reader. Never download or compile a fallback."""

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zipfile


MANIFEST = 'deps/dwarfs-reader/manifest.json'
SOURCE_INPUTS = {
    'include/ipasim/DwarfsReaderBridgeApi.h',
    'src/IpaSimulator/DwarfsReaderBridge.cpp',
    'src/IpaSimulator/dwarfs-reader/CMakeLists.txt',
    'tools/ci/build-dwarfs-reader.ps1',
}


def checked_path(root, name):
    """Manifest paths stay inside the checkout, including through symlinks."""
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError(f'unsafe package path: {name}')
    result = (root / name).resolve()
    if not result.is_relative_to(root.resolve()):
        raise ValueError(f'package path escapes checkout: {name}')
    return result


def verify_hash(data, expected, label):
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        raise ValueError(f'{label} SHA-256 mismatch: expected {expected}, got {actual}')


def read_package(root):
    root = Path(root)
    manifest = json.loads((root / MANIFEST).read_text(encoding='utf-8'))
    if (manifest['schema_version'] != 1 or manifest['abi_version'] != 1 or
            manifest['platform'] != 'windows-x64'):
        raise ValueError('unsupported reader package schema, ABI, or platform')
    if set(manifest['source_files']) != SOURCE_INPUTS:
        raise ValueError('reader source fingerprint set is incomplete or unexpected')
    for name, expected in manifest['source_files'].items():
        # Git checkouts may use CRLF on Windows. Hash canonical LF text.
        data = checked_path(root, name).read_bytes().replace(b'\r\n', b'\n')
        try:
            verify_hash(data, expected, name)
        except ValueError as error:
            raise ValueError(
                f'{error}. Reader inputs changed: explicitly build, test and '
                'repin a new reader package; automatic rebuilding is disabled.'
            ) from error
    if not manifest['notices']:
        raise ValueError('reader package has no license notices')
    for name, expected in manifest['notices'].items():
        verify_hash(checked_path(root, name).read_bytes().replace(b'\r\n', b'\n'),
                    expected, name)

    archive = manifest['archive']
    archive_path = checked_path(root, archive['path'])
    data = archive_path.read_bytes()
    if len(data) != archive['size']:
        raise ValueError(f'reader archive size mismatch: {len(data)} != {archive["size"]}')
    verify_hash(data, archive['sha256'], 'reader archive')
    binary = manifest['binary']
    if binary['name'] != 'IpaSimDwarfsReader.dll':
        raise ValueError('unexpected reader DLL name')
    # Read verified bytes, never extract arbitrary archive paths to disk.
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        entries = package.infolist()
        if len(entries) != 1 or entries[0].filename != binary['name']:
            raise ValueError('reader archive must contain exactly IpaSimDwarfsReader.dll')
        entry = entries[0]
        if (entry.file_size != binary['size'] or entry.file_size > 32 * 1024 * 1024
                or (entry.external_attr >> 16) & 0o170000 == 0o120000):
            raise ValueError('invalid reader archive member size or type')
        dll = package.read(entry)
    verify_hash(dll, binary['sha256'], 'reader DLL')
    if len(dll) < 64 or dll[:2] != b'MZ':
        raise ValueError('reader DLL is not a PE image')
    pe = struct.unpack_from('<I', dll, 0x3c)[0]
    if (pe + 26 > len(dll) or dll[pe:pe + 4] != b'PE\0\0' or
            struct.unpack_from('<H', dll, pe + 4)[0] != 0x8664 or
            not struct.unpack_from('<H', dll, pe + 22)[0] & 0x2000 or
            struct.unpack_from('<H', dll, pe + 24)[0] != 0x20b):
        raise ValueError('reader DLL is not a Windows x64 PE32+ DLL')
    return manifest, dll


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo-root', type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path,
                        help='write the verified DLL here; omit to verify only')
    args = parser.parse_args()
    try:
        manifest, dll = read_package(args.repo_root)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(dll)
        print(f'[dwarfs-reader-package] verified {manifest["package_version"]}: '
              f'{manifest["binary"]["sha256"]}')
        print('[dwarfs-reader-package] prebuilt reader reused; no cache, download, '
              'DwarFS build or vcpkg install required')
        return 0
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f'[dwarfs-reader-package] ERROR: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
