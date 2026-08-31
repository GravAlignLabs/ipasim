# ipaSim Agent Instructions

These instructions apply to the entire repository unless a more specific `AGENTS.md` exists below the file being edited.

## Mission

Extend ipaSim toward running modern ARM64 iOS applications on Windows while preserving the project's original research and architecture where it remains useful.

The goal is correctness and progressively broader compatibility, not merely getting a particular binary past a failure point.

The long-term scaling objective is to derive the **mechanical iOS API/ABI surface from Apple SDK metadata and compiler evidence in bulk**, generate reusable ARM64-to-Win64 bridge records, and keep semantic-provider approval as a separate, explicit layer. Real application execution is a validation and dynamic-discovery mechanism; it is not the primary discovery mechanism for mechanical SDK coverage.

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

Use **two complementary development loops** and do not confuse them.

### Mechanical SDK/API/ABI coverage

Mechanical coverage should be derived in bulk whenever Apple SDK/compiler evidence can establish it safely.

1. Scan the target SDK's TAPI interfaces and headers across the full relevant SDK surface.
2. Join provider/export evidence with exact Clang-backed signatures.
3. Generate AAPCS64 lowering, Win64 carrier lowering, libffi plans, and runtime adapter records for every mechanically supported candidate in deterministic batches.
4. Keep Objective-C metadata, TLS/data symbols, callbacks, variadics, no-prototype declarations, unknown stack placement, and other unsupported ABI classes explicit rather than guessing.
5. Measure mechanical coverage by framework/subsystem and ABI class instead of waiting for an application to execute every symbol.
6. Never treat generated SDK/ABI evidence as semantic implementation approval.

Do **not** require a runtime failure before adding safe SDK-wide mechanical knowledge. Once a generated mechanism is proven in production, prefer SDK-wide or coherent subsystem-wide generation over migrating trivial ABI-known symbols one PR at a time.

### Semantic and runtime compatibility

For behavior that static SDK/compiler evidence cannot prove, work from the **first genuine non-cascading runtime failure**.

1. Reproduce the failure with the smallest public fixture or semantic smoke test available.
2. Identify the actual subsystem boundary: loader, Mach-O, dyld, host bridge, descriptor model, Mach IPC, filesystem, sockets, process APIs, VM, timing, Objective-C/runtime, framework dependency, etc.
3. Implement the behavior generally at the correct subsystem boundary.
4. Add or extend a semantic test that proves the required behavior.
5. Run the public validation workflows in order.
6. Only then advance to the next genuine semantic/runtime boundary.

Prefer subsystem fixes over symbol-by-symbol exceptions when several failures share the same missing abstraction. Runtime failures remain authoritative evidence for dynamic lookup, callbacks, XPC/Mach behavior, Objective-C/Swift runtime behavior, framework lifecycle, and other semantics that static SDK metadata cannot establish.

## Integration cadence

`master` is the **rolling integration baseline**, not a branch that waits for the emulator to be complete.

- Do not keep correct, tested work on a long-lived feature branch merely because adjacent framework/runtime work is still unfinished.
- Open a PR as soon as there is a coherent compatibility increment. Draft PRs are appropriate while that increment is still being validated.
- Merge an increment back to `master` when it is independently correct, reviewable, preserves known-good behavior, and its applicable public tests are green.
- Full iOS compatibility, full framework support, or success with every application is **not** a prerequisite for merging a completed subsystem increment.
- Examples of mergeable increments include an SDK-wide mechanical catalog/generator improvement, a loader semantic, an ABI correction, a Darwin API implementation plus smoke coverage, a descriptor-model improvement, a synthetic fixture, or a CI diagnostic improvement.
- Keep branches short-lived by default. If work spans multiple independent boundaries, split it into sequential PRs instead of keeping one branch open indefinitely.
- After a PR merges, start follow-on work from the updated `master` so later work continuously incorporates what has already landed.
- Record unfinished adjacent work as follow-up issues/PRs rather than holding completed work hostage to future milestones.
- Keep `master` green. If a merge causes a regression, fixing or reverting that regression takes priority over advancing to the next boundary.
- Do not wait for a multi-month or multi-year “project complete” checkpoint before integrating useful work.

The preferred rhythm is:

```text
coherent mechanical batch or genuine semantic boundary
        |
        v
implementation + focused test
        |
        v
public CI / PR diagnostic loop
        |
        v
merge to master when green
        |
        v
next branch starts from updated master
```

## Work claims and agent coordination

Before beginning a substantial compatibility increment, inspect `.github/agent-work/` and open/draft PRs for overlapping work.

For agents or maintainers with trusted write access to this repository:

1. Create one narrow claim file at `.github/agent-work/<short-scope-slug>.yml` using `.github/agent-work/CLAIM_TEMPLATE.yml`.
2. Commit that claim directly to `master` as a **metadata-only** coordination commit such as `claim: mach_msg2 ABI`.
3. Do not include source-code changes in a claim commit.
4. Create the feature branch from the newly updated `master`, so the branch includes its own claim.
5. Implement and test the real change on that branch.
6. Open a focused PR as soon as the increment is coherent.
7. Delete the claim file in the implementation PR so the claim disappears when the work merges.
8. Start any follow-on boundary from the newly updated `master` with a new claim.

This is the one intentional exception to the normal “implementation through PRs” rule: a tiny direct-to-`master` commit may be used to publish **coordination metadata only**. It must never be used to bypass review or CI for source changes.

If work lasts more than 72 hours, update the claim's `updated_utc` and `next_checkpoint` with a small metadata-only `claim update: <slug>` commit. A claim is considered stale after **7 days without an update** and must not reserve a subsystem indefinitely. Before taking over stale work, check for recent branch or PR activity. Maintainers may remove abandoned stale claims.

If work is abandoned intentionally, delete its claim with a small commit such as `release claim: <slug>` metadata commit.

Contributors without direct `master` write access should not be granted write access just to create claims. They should check existing claims, open a draft PR immediately for the narrow boundary they intend to work on, and use that draft PR as the public claim until a maintainer decides whether a claim file should be landed.

Claims are public. Never include private application names, bundle identifiers, paths, RuntimeRoot contents, logs, screenshots, or binary fingerprints. Describe the emulator boundary generically.

See `.github/agent-work/README.md` for the full claim lifecycle.

## Public validation order

Run the applicable public checks in this order:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. **Threaded ARM64 Guest Context** when guest-thread/callback execution is relevant
4. **Compatibility Surface Analyzer** when generated compatibility tooling or fixtures change
5. Optional private/local acceptance only after the public synthetic/core/tooling checks are understood

Public fixtures currently include:

- `HelloBootstrap.ipa` — minimal ARM64 guest; successful execution must return `X0=42`
- `HelloNative.ipa` — untouched minimal iOS executable used to expose the next genuine Apple runtime boundary
- `HelloUIKit.ipa` — small UIKit/Foundation application used to expose framework/runtime boundaries publicly

If private/local testing finds a new semantic boundary, reproduce the semantic requirement with a public synthetic fixture whenever practical before asking outside contributors to debug it.

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
- Scope PRs around independently mergeable engineering increments whenever practical.
- Preserve useful subsystem history; meaningful engineering milestones are preferable to one opaque mega-commit.
- Do not manufacture empty commits merely to inflate activity.
- Do not merge private acceptance data into public history.
- When one increment is ready, merge it; do not keep adding unrelated future work to the same PR.
- Continue unfinished work in a follow-up PR based on the newly updated `master`.
- For non-trivial agent work, identify the active claim in the PR and remove that claim as part of the merge.

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
