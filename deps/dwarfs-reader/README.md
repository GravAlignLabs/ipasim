# Pinned Windows x64 DwarFS reader

`windows-x64/reader.zip` is the **already compiled and tested** reader from
[successful public acceptance run 33878644011](https://github.com/GravAlignLabs/ipasim/actions/runs/33878644011).
It is stored in ordinary Git, not LFS, Actions cache, or an expiring artifact.
It contains only `IpaSimDwarfsReader.dll`. No Apple RuntimeRoot bytes are included.

The 1,883,952-byte ZIP is unchanged from artifact `9939676457`; its SHA-256 is
`fba3031eceee9a5dcaf0c89c6029591612d301867048a9c84bdcd21fd8d58a5a`.
The 4,329,472-byte DLL has SHA-256
`a6d7477479c632178952de801082f3d0992e900919cf7cd510fb28a2c5cd0cd0`.

## Use without building the reader

From a repository checkout with Python 3.10 or newer:

```powershell
python tools/ci/prepare-dwarfs-reader.py --output C:/ipasim/IpaSimDwarfsReader.dll
```

The preparer verifies the manifest, original build inputs, ZIP, DLL, Windows x64
PE format, and accompanying notices before writing the DLL. It works offline.
Missing files, altered inputs, or checksum failures stop with an actionable error.
There is **no download or source-build fallback**.

CI then compiles just `DwarfsRuntimeRootStore.cpp` and its current smoke assertions
with the small `src/IpaSimulator/dwarfs-reader-smoke` CMake project. It does not
build DwarFS, install vcpkg dependencies, or require the original MSVC patch version
to match the current runner. The consumer still tests the real DLL's C ABI,
Darwin-only pathname handling, and failure behavior before full image acceptance.

This fixes reader reuse only. The separate multi-gigabyte RuntimeRoot image still
has its own cache and preparation policy; it is not distributed in this package.

## Public API

The versioned C ABI is documented by
[`include/ipasim/DwarfsReaderBridgeApi.h`](../../include/ipasim/DwarfsReaderBridgeApi.h).
Use `LoadLibraryW` / `GetProcAddress` to resolve `ipasim_dwarfs_reader_abi`,
`ipasim_dwarfs_reader_open`, `ipasim_dwarfs_reader_read`,
`ipasim_dwarfs_reader_free`, and `ipasim_dwarfs_reader_close`.
Check ABI version 1 before opening an image. Paths inside the image are absolute
Darwin UTF-8 paths, not Windows paths. Free returned buffers through the reader's
free function and close handles through its close function. See
[`DwarfsRuntimeRootStore.cpp`](../../src/IpaSimulator/DwarfsRuntimeRootStore.cpp)
for a checked consumer. The DLL statically links its non-system dependencies.

## Deliberate reader updates

Ordinary emulator, store-consumer, or smoke-test changes reuse this package.
Changing the bridge, ABI header, original CMake build, or dependency build recipe
requires an explicit new reader package. Do not merely edit the manifest to match
new code: that would claim an untested binary implements it.

1. Run `tools/ci/build-dwarfs-reader.ps1` deliberately on Windows with a new empty
   `-ReaderCacheRoot`, the public fixture, and the pinned upstream inputs. This
   source-build recipe is retained for reproducibility; acceptance never calls it.
2. Pass the reader smoke and full applicable public acceptance tests.
3. Replace the ZIP, update checksums, source fingerprints, provenance, and notices
   in a focused PR. Preserve the old version through Git history.
4. Run `python -m unittest discover -s tools/ci/tests -p test_dwarfs_reader_package.py`
   and the current-source Windows smoke before merge.

`manifest.json` records the original public run, source commit, DwarFS source
archive, vcpkg baseline, MSVC build, and dependency versions observed in that run.
The upstream source archive SHA was independently verified while promoting the
existing artifact. No reader recompilation was performed to create this package.

## Notices

Distribute this directory's manifest, documentation, and `notices/` together with
the DLL. DwarFS reader libraries use MIT licensing; the GPL DwarFS image-writing
tools are not part of this package. The notices include the upstream bundled
components and a conservative set of build-dependency notices, including packages
used only at build time. They do not assert that every built package is linked
into the DLL. Source locations are recorded in `notice-sources.json`.

This product includes software developed by the OpenSSL Project for use in the
OpenSSL Toolkit (http://www.openssl.org/), cryptographic software written by
Eric Young (eay@cryptsoft.com), and software written by Tim Hudson
(tjh@cryptsoft.com).
