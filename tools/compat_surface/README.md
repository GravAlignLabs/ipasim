# Compatibility surface analyzer

The compatibility-surface tools are offline, target-neutral metadata analyzers for
the modern ipaSim compatibility pipeline. They turn information that would
otherwise live only in runtime logs, SDK text stubs, or SDK headers into
deterministic JSON that tools, agents, and later code generators can consume.

They are intentionally evidence-oriented:

- they never rewrite a Mach-O image;
- they never patch or remap an import;
- they never mark an unsupported API as successful;
- they never infer a function signature from symbol spelling;
- SDK/compiler evidence never grants semantic-provider approval;
- parse ambiguities fail explicitly instead of silently normalizing evidence.

## Why this exists

Jan Joneš's original ipaSim thesis used SDK metadata and generated wrappers so
calling-convention plumbing did not have to scale one hand-written function at a
time. The modern ARM64 compatibility engine follows that same scaling principle,
with a stronger separation between mechanical ABI evidence and runtime semantics.

The tooling supports two related views:

- an **SDK-wide planning view**, built before any particular application executes;
- an **import-scoped validation view**, showing what a particular Mach-O requires.

The SDK-wide view is now the preferred source for broad mechanical API/ABI
coverage. Runtime/import evidence remains essential for validating a specific
application and for semantics that static SDK data cannot establish.

The manifests can be joined with:

- SDK `.tbd` export/re-export metadata;
- SDK/header-derived function signatures;
- ARM64 and Win64 compiler-lowered ABI evidence;
- generated libffi adapter plans and runtime records;
- explicit semantic-provider approval data;
- runtime hit counts and transition timing.

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

`tbd_surface.py` converts Apple TAPI `.tbd` metadata into a deterministic SDK
manifest. It can scan one file or the complete SDK tree; the whole-SDK scan is
the foundation for SDK-wide compatibility planning.

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
that directly provide it. That index is provider metadata only. It does not
choose an implementation or claim that a symbol is callable.

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
root is scanned. That is the mode used to build a broad SDK-wide C signature
universe. It can be substantially slower than a subsystem-targeted scan.

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
directly with both the Mach-O import surface and TAPI direct-export index without
assuming that prefixing a source identifier is always sufficient.

## SDK-wide typed compatibility catalog

`sdk_catalog.py` is the SDK-scale join. It consumes the **complete target-matching
TAPI symbol index** plus the Clang header-signature surface and produces a
`typed-sdk-catalog` without requiring any Mach-O import manifest.

That distinction is deliberate. Mechanical compatibility knowledge should not
wait for a particular application to fail on each symbol one at a time.

Build a catalog:

```text
python tools/compat_surface/sdk_catalog.py ^
  --tapi ios-16.5-sdk-surface.json ^
  --headers ios-16.5-header-signatures.json ^
  --output ios-16.5-typed-sdk-catalog.json
```

To also feed all typed global C candidates into the already validated AAPCS64 ABI
generator, request a mechanical projection:

```text
python tools/compat_surface/sdk_catalog.py ^
  --tapi ios-16.5-sdk-surface.json ^
  --headers ios-16.5-header-signatures.json ^
  --output ios-16.5-typed-sdk-catalog.json ^
  --abi-inventory-output ios-16.5-sdk-abi-input.json

python tools/compat_surface/abi_surface.py ^
  --inventory ios-16.5-sdk-abi-input.json ^
  --header-root C:\path\to\iPhoneOS16.5.sdk ^
  --sdk-root C:\path\to\iPhoneOS16.5.sdk ^
  --output ios-16.5-aapcs64-surface.json
```

The ABI projection deliberately contains:

- every SDK catalog row classified as a typed global C function;
- exact Clang signature evidence;
- direct TAPI provider evidence;
- **zero Mach-O runtime requirements**;
- no semantic-provider approval.

It is marked `scope: sdk-wide-mechanical-projection` so downstream tooling and
humans can distinguish it from an application/import-scoped inventory.

The catalog keeps the following categories separate instead of flattening them
into presumed functions:

- typed global C functions;
- untyped globals, which may be data or functions not described by the header surface;
- Objective-C class/exception/ivar metadata;
- thread-local exports;
- mixed-kind evidence;
- weak versus strong exports;
- symbols with multiple direct providers;
- header signatures that have no target-matching TAPI export.

### SDK catalog manifest v1

The high-level shape is:

```json
{
  "schema_version": 1,
  "kind": "typed-sdk-catalog",
  "targets": {
    "clang": "arm64-apple-ios16.0",
    "tapi": "arm64-ios"
  },
  "summary": {
    "interface_count": 0,
    "symbol_count": 0,
    "global_symbol_count": 0,
    "typed_global_symbol_count": 0,
    "untyped_global_symbol_count": 0,
    "weak_symbol_count": 0,
    "objc_symbol_count": 0,
    "thread_local_symbol_count": 0,
    "mixed_kind_symbol_count": 0,
    "multi_provider_symbol_count": 0,
    "header_signature_count": 0,
    "orphan_header_signature_count": 0
  },
  "symbols": [],
  "orphan_header_signatures": []
}
```

A catalog row becomes `callable_c_candidate: true` only when its target-matching
TAPI kind is exactly `global` and an exact Clang signature exists. The catalog
may retain a coincidentally matching header signature for non-C TAPI metadata,
but that metadata remains non-callable and is excluded from the ABI projection.

This is still mechanical evidence only. The next layer must compare the generated
mechanical surface with an independently maintained semantic-provider inventory;
SDK presence is never semantic approval.

## Import-scoped typed compatibility inventory

`inventory_surface.py` remains useful for a different question: what does a
**particular Mach-O set** require, and does that import/provider requirement agree
with the SDK evidence?

It joins:

```text
Mach-O import requirement
        +
TAPI provider/re-export evidence
        +
Clang header signature evidence
        -> typed compatibility inventory
```

Use this view for application validation, provider/re-export mismatch diagnosis,
weak/special ordinals, and importer-specific requirements. Do not require an
import-scoped inventory before generating SDK-wide mechanical coverage.

## Downstream ABI and bridge surfaces

The existing downstream tools remain the mechanical execution pipeline:

```text
typed mechanical inventory
        -> abi_surface.py            (AAPCS64)
        -> win64_abi_surface.py      (Win64 carrier ABI)
        -> bridge_plan.py            (libffi-oriented repacking plan)
        -> runtime_adapter_table.py  (runtime records / generated C++)
```

These tools deliberately classify unsupported cases instead of guessing. Current
AAPCS64 status classes include generated bridge candidates, callback-runtime,
variadic-runtime, manual ABI, and no-prototype cases. Exact unknown guest stack
placement remains unknown until proven.

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

The SDK catalog tests prove that symbols do not need to appear in a Mach-O import
manifest before entering the mechanical map. They also cover typed/untyped global
separation, Objective-C/TLS non-callability, target filtering, weak/strong and
multi-provider evidence, deterministic output, SDK-wide ABI projection, path
privacy, and fail-closed inconsistent callable records.

Malformed ordinals, unsafe offsets, ambiguous TAPI metadata, invalid YAML, Clang
parse failures, conflicting header signatures, and inconsistent SDK catalog facts
fail explicitly.

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
Swift declarations, every record layout/field offset, or runtime behavior.

The SDK catalog does not turn untyped globals into functions, does not interpret
Objective-C/TLS metadata as callable C, and does not approve semantic providers.
The SDK-wide ABI projection is intentionally a schema adapter into the existing
AAPCS64 generator; its empty requirement list is not evidence that any application
requested or executed those symbols.

The major remaining scaling layers are therefore:

```text
SDK-wide typed catalog
        -> bulk AAPCS64 / Win64 / libffi coverage report
        -> machine-readable semantic-provider inventory
        -> generated explicit production route data
        -> runtime validation and dynamic-discovery feedback
```

Runtime telemetry, callbacks, variadics, Objective-C/Swift semantics, XPC/Mach
behavior, framework lifecycle, graphics, and other semantic implementations must
remain separate from the static evidence pipeline.
