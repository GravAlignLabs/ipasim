# Compatibility surface analyzer

The compatibility-surface tools are offline, target-neutral metadata analyzers for
the modern ipaSim compatibility pipeline. They turn information that would
otherwise live only in runtime logs, SDK text stubs, or SDK headers into
deterministic JSON that tools, agents, and later code generators can consume.

They are intentionally **diagnostic only**:

- they never rewrite a Mach-O image;
- they never patch or remap an import;
- they never mark an unsupported API as successful;
- they never infer a function signature from symbol spelling;
- parse ambiguities fail explicitly instead of silently normalizing evidence.

## Why this exists

Jan Joneš's original ipaSim thesis used SDK metadata and generated wrappers so
calling-convention plumbing did not have to scale one hand-written function at a
time. The modern ARM64 work already has runtime import inventories, but that
knowledge has historically been mostly emitted as text during a run.

These tools start rebuilding the machine-readable half of that architecture so
the same surface can later be joined with:

- SDK `.tbd` export/re-export metadata;
- SDK/header-derived function signatures;
- `SysTranslator` ABI registrations;
- `IpaSimDarwinHost.dll` exports;
- runtime hit counts and transition timing;
- generated ARM64 <-> Windows x64 bridge metadata.

The manifests are evidence about what binaries and SDKs expose. They are not
evidence that an implementation is semantically correct.

## ARM64 Mach-O import surface

`compat_surface.py` converts ARM64 Mach-O dependency ordinals and
`LC_DYLD_CHAINED_FIXUPS` import metadata into deterministic JSON.

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

### Mach-O manifest v1

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
    "providers": []
  },
  "images": []
}
```

Each image records ordered dependencies and its resolved import bindings. A
positive ordinal is resolved to the exact corresponding dependency. Known
negative dyld ordinals are classified as `main-executable`, `flat-lookup`, or
`weak-lookup`. Unknown special ordinals are preserved rather than normalized
away.

## Apple TAPI SDK knowledge surface

`tbd_surface.py` converts Apple TAPI `.tbd` metadata into a second deterministic
manifest designed to join with the Mach-O import surface.

Install the one parser dependency:

```text
python -m pip install PyYAML==6.0.2
```

Analyze one or more text stubs:

```text
python tools/compat_surface/tbd_surface.py path/to/libSystem.B.tbd
```

Analyze an extracted or checked-out SDK tree recursively:

```text
python tools/compat_surface/tbd_surface.py ^
  --sdk-root C:\path\to\iPhoneOS16.5.sdk ^
  --output ios-16.5-sdk-surface.json
```

The default target set is `arm64-ios` plus `arm64e-ios`. Override it only when
you deliberately want a different TAPI target:

```text
python tools/compat_surface/tbd_surface.py path/to/file.tbd ^
  --target arm64-ios
```

The parser understands the legacy untagged text-stub shape plus tagged TAPI
v2/v3 and modern v4 multi-document files. This matters for files such as
`libSystem.B.tbd`, which may contain many install names in one text stream.

For each matching interface it preserves:

- install name;
- TAPI format generation;
- current and compatibility versions when present;
- ARM64/ARM64e target membership;
- direct global, weak, thread-local, and Objective-C export categories;
- direct re-export relationships;
- source paths relative to the supplied SDK root.

It also emits a `symbol_index` that maps each **direct export** to all interfaces
that directly provide it. That index is candidate-provider metadata only. It
does not chase re-export graphs, choose an implementation, or claim that a
symbol is callable.

### TAPI SDK manifest v1

The normalized shape is:

```json
{
  "schema_version": 1,
  "kind": "tapi-sdk-surface",
  "summary": {
    "interface_count": 2,
    "export_count": 10,
    "weak_export_count": 1,
    "objc_export_count": 1,
    "reexport_count": 1,
    "unique_symbol_count": 9
  },
  "interfaces": [],
  "symbol_index": []
}
```

Duplicate files with the same install name are merged only when their version
metadata is coherent. Conflicting format/current/compatibility versions fail
explicitly rather than choosing one arbitrarily.

## Clang-backed SDK header signature surface

`header_surface.py` adds the next mechanical layer: canonical C function type
metadata recovered from SDK headers by Clang itself instead of source regexes or
symbol-name guesses.

The default target is `arm64-apple-ios16.0`. The analyzer invokes Clang twice per
header: the first AST pass discovers externally visible C declarations owned by
that header, and a small generated `__typeof__` probe asks Clang to expose the
underlying `FunctionProtoType`/`FunctionNoProtoType` tree. That second type tree
lets typedefs be resolved structurally while preserving pointers, function
pointers, records, arrays, qualifiers, variadic state, and calling convention.

Analyze explicit headers:

```text
python tools/compat_surface/header_surface.py path/to/header.h ^
  --output header-signatures.json
```

Analyze selected public headers inside an SDK root:

```text
python tools/compat_surface/header_surface.py ^
  --sdk-root C:\path\to\iPhoneOS16.5.sdk ^
  --relative-header usr/include/unistd.h ^
  --relative-header usr/include/pthread.h ^
  --output ios-16.5-header-signatures.json
```

If `--sdk-root` is supplied without `--relative-header`, every `.h` below the SDK
root is scanned. That is useful for offline indexing but can be substantially
slower than targeting the subsystem currently under investigation.

Feature macros or other parse settings can be passed explicitly without changing
the analyzer:

```text
python tools/compat_surface/header_surface.py path/to/header.h ^
  --clang-arg=-D__DARWIN_C_LEVEL=200809L
```

For each C-ABI function the manifest preserves:

- the exact Darwin Mach-O symbol name reported by Clang;
- all coherent C identifier names and function-type spellings found in headers;
- Clang calling convention;
- prototype/no-prototype and variadic state;
- a structural canonical return type;
- structural canonical parameter types plus original typedef spellings/names;
- source header, line, and column provenance.

Static functions and declarations that merely arrived through an included header
are not claimed as exports of the header being analyzed. C++-mangled declarations
are counted and intentionally left out of this C-ABI increment. Coherent duplicate
declarations merge; conflicting signatures for the same Mach-O symbol fail
explicitly.

SDK-root paths are emitted relative to the SDK. Absolute local paths and temporary
probe paths are sanitized from Clang failure diagnostics.

### Header signature manifest v1

The normalized shape is:

```json
{
  "schema_version": 1,
  "kind": "header-signature-surface",
  "target": "arm64-apple-ios16.0",
  "summary": {
    "header_count": 2,
    "declaration_count": 20,
    "unique_symbol_count": 18,
    "variadic_symbol_count": 1,
    "no_prototype_symbol_count": 0,
    "skipped_cxx_declaration_count": 0,
    "skipped_static_declaration_count": 4
  },
  "signatures": []
}
```

The `symbol` field uses Clang's actual Darwin mangled name, so it can be joined
directly with the Mach-O import surface and TAPI direct-export index without
assuming that prefixing a source identifier is always sufficient.

## Tests

Run the complete analyzer test suite:

```text
clang --version
python -m pip install PyYAML==6.0.2
python -m unittest discover -s tools/compat_surface -p "test_*.py" -v
```

The Mach-O tests build synthetic thin/fat ARM64 fixtures and verify all three
modern chained-import encodings currently supported:

- `DYLD_CHAINED_IMPORT`
- `DYLD_CHAINED_IMPORT_ADDEND`
- `DYLD_CHAINED_IMPORT_ADDEND64`

The TAPI tests cover legacy, v3, and v4 multi-document stubs, target filtering,
weak/Objective-C categories, re-exports, direct-provider indexing, duplicate
conflict detection, deterministic output, and SDK-root path privacy.

The header tests use synthetic C/Objective-C-compatible headers and real Clang AST
metadata to cover typedef canonicalization, function-pointer returns, variadics,
record returns, old-style no-prototype declarations, duplicate/conflicting
signatures, include/static filtering, deterministic output, and SDK-root path
privacy.

Malformed ordinals, unsafe offsets, ambiguous TAPI metadata, invalid YAML, Clang
parse failures, and conflicting header signatures fail explicitly.

## Privacy

Do not publish RuntimeRoot contents, private application manifests, or local
absolute paths to public issues, pull requests, CI logs, or artifacts. Use
repository-generated synthetic fixtures for public reproduction.

SDK manifests should be generated from redistributable/public SDK metadata when
they are intended for public collaboration.

## Deliberate limits

The Mach-O analyzer does not yet parse legacy `LC_DYLD_INFO` binds, exports
tries, Objective-C image metadata, or Swift metadata.

The TAPI analyzer deliberately does **not** infer C/Objective-C prototypes,
AAPCS64 argument classes, behavior, side effects, callbacks, or implementation
correctness. `.tbd` files establish symbol/provider/version/re-export evidence.

The header analyzer currently indexes externally visible **C ABI function
declarations**. It does not yet index Objective-C methods/properties, C++ overloads,
Swift declarations, record layout/field offsets, AAPCS64 register/stack classes,
or generate host-call implementations. Those are separate layers so a failure in
one source of evidence cannot silently become a compatibility claim.

The next useful join is therefore:

```text
Mach-O import requirement
        +
TAPI provider/version evidence
        +
Clang header signature/type evidence
        -> typed compatibility inventory
```

Generated AAPCS64 <-> Win64 bridges, runtime telemetry, callbacks, and semantic
implementations should consume that inventory rather than replace its evidence.
