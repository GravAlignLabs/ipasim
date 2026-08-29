# ipaSim Agent Instructions

These instructions apply to the entire repository unless a more specific `AGENTS.md` exists below the file being edited.

## Mission

Extend ipaSim toward running modern ARM64 iOS applications on Windows while preserving the project's original research and architecture where it remains useful.

The goal is correctness and progressively broader compatibility, not merely getting a particular binary past a failure point.

## Non-negotiable rules

- Do **not** monkey patch, runtime-swap methods, or add hidden compatibility hooks.
- Do **not** fake successful Darwin/iOS behavior. If a semantic mapping is not defensible, fail explicitly with a useful diagnostic.
- Do **not** suppress compiler, linker, loader, ABI, runtime, or semantic-test failures.
- Do **not** add application-specific names, paths, bundle identifiers, binary fingerprints, private logs, or one-off hacks for private applications.
- Keep public reproductions target-neutral and based on repository-generated synthetic fixtures.
- Prefer real Windows-backed semantics when Windows provides an appropriate equivalent.
- Preserve known-good behavior and existing test coverage while extending compatibility.
- Do not invent Darwin return values, process metadata, descriptor semantics, timing behavior, or kernel state merely to make a caller continue.
- When an unsupported API is reached, explicit `ENOTSUP`/failure is preferable to a false success unless the API contract itself requires another defensible behavior.

## Current architecture

The active modernization path is:

- Windows x64 host
- ARM64/AArch64 guest execution through Unicorn
- AAPCS64 guest ABI handling
- modern ARM64 Mach-O loading
- modern dyld behavior including chained fixups, export tries, dependency ordinals, and RuntimeRoot resolution
- guest-to-host translation through explicit Darwin compatibility bridges
- in-process Mach IPC compatibility
- Windows-backed filesystem, process, socket, time, VM, credential, and descriptor behavior where defensible

The historical WinObjC/32-bit paths remain part of the repository, but do not force legacy assumptions into the modern x64 ARM64 core.

## Development strategy

Work from the **first genuine non-cascading failure**.

1. Reproduce the failure with the smallest public fixture or semantic smoke test available.
2. Identify the actual subsystem boundary: loader, Mach-O, dyld, ABI, host bridge, descriptor model, Mach IPC, filesystem, sockets, process APIs, VM, timing, Objective-C/runtime, framework dependency, etc.
3. Implement the behavior generally at the correct subsystem boundary.
4. Add or extend a semantic test that proves the required behavior.
5. Run the public validation workflows in order.
6. Only then advance to the next compatibility boundary.

Prefer subsystem fixes over symbol-by-symbol exceptions when several failures share the same missing abstraction.

## Public validation order

Run these in order:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. Optional private/local acceptance only after the public synthetic/core checks are understood

Public fixtures currently include:

- `HelloBootstrap.ipa` — minimal ARM64 guest; successful execution must return `X0=42`
- `HelloNative.ipa` — untouched minimal iOS executable used to expose the next genuine Apple runtime boundary
- `HelloUIKit.ipa` — small UIKit/Foundation application used to expose framework/runtime boundaries publicly

If private/local testing finds a new boundary, reproduce the semantic requirement with a public synthetic fixture whenever practical before asking outside contributors to debug it.

## PR-based CI diagnostics

The Windows ARM64 Core workflow intentionally self-reports useful failures to pull requests.

For the emulator-core build:

- capture the verbose build log
- preserve the real failing exit code
- extract the first actionable compiler/linker/build diagnostic
- write the diagnostic to the Actions step summary
- create or update **one persistent PR comment** identified by `<!-- ipasim-core-build-diagnostic -->`
- include the failing commit SHA
- update that same comment on later pushes instead of creating duplicates
- after publishing the diagnostic, fail the workflow normally

Never convert a failed build into success merely so diagnostic publishing can run.

If extending this mechanism to more smoke/runtime stages, preserve the same rule: **publish the useful diagnostic, then fail normally**.

## Coding expectations

- Keep guest and host concepts distinct. A Windows handle is not automatically a Darwin descriptor, port, `pthread_t`, Mach task, or VM object.
- Maintain coherent guest-visible namespaces instead of scattering ad hoc integer ranges and special cases.
- Translate data structures field-by-field; validate sizes and flavors explicitly.
- Validate ABI signatures before wiring host calls into `SysTranslator`.
- Treat exported data symbols differently from callable symbols.
- Track host resources when Darwin semantics require lifetime/size/identity information that the host API alone cannot reconstruct later.
- Prefer comments that explain semantic differences between Darwin and Windows rather than comments that restate code.
- Avoid broad refactors during a compatibility-boundary fix unless the failure demonstrates that the abstraction itself is wrong.

## External reference projects

Projects such as touchHLE may be used as architectural references for subsystem design, especially descriptor tables, guest object identity, timing models, and high-level iOS semantics.

Do not copy assumptions blindly. touchHLE targets a different iOS era and architecture; adapt useful patterns to ipaSim's modern ARM64/Windows model and verify them against public Apple/XNU interfaces or synthetic tests.

## Pull requests and commits

- Keep PRs target-neutral and evidence-driven.
- Explain the actual compatibility boundary being addressed.
- Include the test that proves the change.
- Preserve useful subsystem history; meaningful engineering milestones are preferable to one opaque mega-commit.
- Do not manufacture empty commits merely to inflate activity.
- Do not merge private acceptance data into public history.

## Privacy boundary

Public CI, issues, discussions, commits, and PRs must not expose private acceptance applications or their identifying data.

Do not commit or post:

- private IPAs
- private application names or bundle identifiers
- private local paths
- screenshots containing private application information
- RuntimeRoot binary contents
- private application logs

Use repository-generated synthetic fixtures and generic tester scripts for public collaboration.

## Scoped instructions

For changes under `src/IpaSimulator/`, also follow `src/IpaSimulator/AGENTS.md`.
