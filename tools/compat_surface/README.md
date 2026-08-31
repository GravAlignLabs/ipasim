# Compatibility surface analyzer

`compat_surface.py` is an offline, target-neutral analyzer for the modern ipaSim
compatibility pipeline. It converts ARM64 Mach-O dependency and
`LC_DYLD_CHAINED_FIXUPS` import metadata into deterministic JSON that tools,
agents, and later code generators can consume.

It is intentionally **diagnostic only**:

- it never rewrites a Mach-O image;
- it never patches or remaps an import;
- it never marks an unsupported API as successful;
- parse ambiguities fail explicitly with a `[compat-surface] ERROR:` diagnostic.

The first version focuses on the mechanical information the loader can know
without guessing API behavior: dependency ordinals, import names, weak imports,
special ordinals, addends, and resolved provider install names.

## Why this exists

Jan Joneš's original ipaSim thesis used SDK metadata and generated wrappers so
calling-convention plumbing did not have to scale one hand-written function at a
time. The modern ARM64 work already has runtime import inventories, but that
knowledge is still mostly emitted as text during a run.

This tool starts turning the same information into a stable machine-readable
surface that can later be joined with:

- SDK `.tbd` export/re-export metadata;
- SDK/header-derived function signatures;
- `SysTranslator` ABI registrations;
- `IpaSimDarwinHost.dll` exports;
- runtime hit counts and transition timing;
- generated ARM64 <-> Windows x64 bridge metadata.

The JSON manifest is evidence about what an image asks for. It is not evidence
that an implementation is semantically correct.

## Usage

Analyze explicit images:

```text
python tools/compat_surface/compat_surface.py path/to/libA.dylib path/to/libB.dylib
```

Analyze the three simulator libSystem companion images under a local
`RuntimeRoot`:

```text
python tools/compat_surface/compat_surface.py --runtime-root C:\path\to\RuntimeRoot
```

Analyze selected RuntimeRoot-relative images:

```text
python tools/compat_surface/compat_surface.py ^
  --runtime-root C:\path\to\RuntimeRoot ^
  --relative-image usr/lib/system/libsystem_sim_kernel.dylib ^
  --relative-image usr/lib/system/libsystem_sim_pthread.dylib ^
  --output compat-surface.json
```

Absolute local paths are intentionally not written into the manifest.
RuntimeRoot inputs are represented by their relative path, and direct image
inputs are represented by basename only.

Do not publish RuntimeRoot contents or private-application manifests to public
issues, pull requests, CI logs, or artifacts. Use repository-generated
synthetic fixtures for public reproduction.

## Manifest v1

The top-level structure is:

```json
{
  "schema_version": 1,
  "summary": {
    "image_count": 1,
    "dependency_count": 2,
    "import_count": 3,
    "weak_import_count": 1,
    "special_ordinal_import_count": 0,
    "providers": [
      {
        "provider": "/usr/lib/system/libsystem_kernel.dylib",
        "import_count": 3
      }
    ]
  },
  "images": []
}
```

Each image records ordered dependencies and its resolved import bindings. A
positive ordinal is resolved to the exact corresponding dependency. Known
negative dyld ordinals are classified as `main-executable`, `flat-lookup`, or
`weak-lookup`. Unknown special ordinals are preserved rather than normalized
away.

## Tests

Run:

```text
python -m unittest discover -s tools/compat_surface -p "test_*.py" -v
```

The tests build synthetic thin/fat ARM64 Mach-O fixtures in memory and verify
all three chained-import encodings currently supported by modern dyld:

- `DYLD_CHAINED_IMPORT`
- `DYLD_CHAINED_IMPORT_ADDEND`
- `DYLD_CHAINED_IMPORT_ADDEND64`

Malformed ordinals and unsafe offsets fail explicitly.

## Deliberate limits of v1

This increment does not parse legacy `LC_DYLD_INFO` binds, exports tries, SDK
headers, Objective-C metadata, or Swift metadata. It also does not infer a
function signature from a symbol name.

Those are follow-on data sources for the compatibility runtime. They should be
added as independently tested inputs rather than hidden fallbacks.
