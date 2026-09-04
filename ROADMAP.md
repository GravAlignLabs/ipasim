# ipaSim runtime compatibility roadmap

This document records the **future semantic/runtime work** for the modern ARM64-on-Windows path. It complements `README.md` and `AGENTS.md`; it is not a promise to implement APIs in a fixed symbol-by-symbol order.

The project uses two complementary evidence loops:

1. **mechanical SDK/API/ABI coverage** is derived in bulk from Apple SDK metadata and compiler evidence; and
2. **semantic/runtime compatibility** advances from the first genuine non-cascading runtime boundary, implemented as a coherent subsystem with public regression coverage.

Generated ABI knowledge never grants semantic approval by itself.

## Current checkpoint — September 3, 2026

The latest merged storage/runtime architecture checkpoint is **PR #77**.

Important merged state:

- PR #67 completed the previously top-priority Darwin pthread-core lifecycle increment;
- PR #68 added a pinned public third-party IPA regression workload;
- PRs #69, #70, and #73 established the trusted GitHub-hosted iOS 18.5 RuntimeRoot `tar + zstd` cross-OS baseline;
- PRs #74 and #75 tested a complete WIM as an isolated read-only mount experiment while keeping diagnostic publishing secondary to real failure;
- PR #76 introduced `RuntimeRootStore`, separating guest Darwin install names and immutable RuntimeRoot bytes from Windows host paths; and
- PR #77 proved a DwarFS-backed store can read a Darwin pathname containing an NTFS-invalid `:` component directly on Windows without mounting, extracting, renaming, or falling back.

The current generated production route set remains the explicitly approved 11-symbol process-identity and descriptor-I/O set documented in `README.md`.

### Current trusted RuntimeRoot baseline

The frozen historical trusted transport is:

```text
GitHub-hosted macOS RuntimeRoot
        -> complete tar stream
        -> zstd -1 -T0
        -> cross-OS Actions cache
        -> Windows restore + SHA-256 verification
        -> complete directory extraction
        -> existing tester / IpaProbe path
```

That workflow remains unchanged while image-backed RuntimeRoot work is validated. The complete archive is now proven to contain Darwin names and hard-link/symlink topology that NTFS cannot materialize exactly: an exact-head extraction attempt stopped before ipaSim with 15,339 hard-link creation errors and 75 rejected link-path errors. It therefore cannot serve as a full-namespace exact-parity oracle. Under the Priority 0 pass criterion below, an image backend proves the storage increment by reaching a genuine loader/runtime boundary later than this host-filesystem stop without changing RuntimeRoot namespace or content semantics.

### Current active checkpoint

Draft **PR #78** is the immediate storage milestone: build the complete pinned iOS 18.5 (22F77) RuntimeRoot as one DwarFS image on macOS, restore that image on Windows, and feed the exact-head ipaSim loader through `RuntimeRootStore` with **no RuntimeRoot mount or full extraction**.

PR #78 is an experiment until it is independently correct and green. Do not document or depend on its behavior as merged functionality before that happens.

## Architectural rules for future work

A missing symbol is evidence of a boundary, not automatically the unit of implementation.

When several failures belong to one Darwin/XNU abstraction, implement the abstraction rather than accumulating aliases or independent shims.

```text
Apple SDK / compiler evidence
          |
          v
mechanical ABI description
          |
          v
explicit semantic-provider approval
          |
          v
Darwin compatibility subsystem
          |
          v
ARM64 guest execution
          |
          v
public synthetic regression proof
```

Every subsystem must remain target-neutral and fail closed when a required semantic cannot be defended.

Runtime evidence may reorder the priorities below. The numbering expresses architectural dependency and expected pressure, not permission to skip an earlier real failure.

## Priority 0 — prove complete image-backed RuntimeRoot acceptance

Finish the active full-RuntimeRoot DwarFS proof before making the image path authoritative.

Required proof:

- source the same pinned GitHub-hosted Xcode 16.4 / iOS 18.5 build 22F77 RuntimeRoot used by trusted acceptance;
- include the **complete** RuntimeRoot with no path exclusions, filename sanitization, renaming, or hidden fallback;
- produce one deterministic/verifiable DwarFS image on macOS;
- restore the exact image on real Windows and verify its identity before use;
- use the validated DwarFS reader bridge rather than extracting the image tree;
- explicitly select the image-backed store; do not guess between directory and image sources;
- run the exact-head Windows Core tester and pinned public IPA through the real loader;
- preserve real loader diagnostics and fail normally if the image-backed path reaches an error;
- preserve the existing directory/zstd baseline unchanged during the experiment; and
- preserve the public Synthetic, Core, Threaded, and DwarFS reader proof baselines.

### Pass criterion

The image-backed path does not need to make an application run farther than the known-good directory path to prove storage parity. It must reach the **same or a later genuine compatibility boundary** without changing RuntimeRoot namespace/content semantics to do so.

If it reaches an earlier image/store-specific error, that error becomes the next storage boundary to fix.

### Why this is Priority 0

The RuntimeRoot contains hundreds of thousands of objects and includes Darwin path components that Windows cannot represent as ordinary NTFS names. The successful PR #77 proof demonstrates that ipaSim can avoid that host-path requirement entirely.

The design principle is:

> Keep guest Darwin pathname identity separate from host storage representation.

Windows should not have to materialize Apple's complete filesystem namespace before ipaSim can resolve a framework or dylib.

## Priority 1 — make RuntimeRootStore the complete read-side source of truth

A successful full-image loader proof is not yet sufficient for production switchover because some pre-load analysis still assumes an extracted directory.

After Priority 0 proves parity, route all read-only RuntimeRoot consumers through the same immutable source boundary.

Required work includes:

- move static dependency/closure inspection onto `RuntimeRootStore` or a store-backed analysis interface;
- move the host-import inventory preflight off direct directory traversal where it consumes RuntimeRoot bytes;
- ensure symlink resolution and source identity are consistent across directory and image stores;
- preserve deterministic missing-path and invalid-path diagnostics;
- preserve the known-good directory backend as a reference implementation during migration;
- add parity tests that feed the same synthetic RuntimeRoot through both backends and compare the relevant loader/audit results; and
- make backend selection explicit rather than introducing auto-detection or a silent extraction fallback.

Only after all authoritative read-side consumers use the abstraction should the project consider making an image backend the trusted default.

## Priority 2 — production RuntimeRoot image switchover and performance hardening

If Priorities 0 and 1 are green, evaluate replacing full Windows RuntimeRoot extraction in trusted acceptance and normal tester workflows.

Acceptance criteria for a production switch:

- no loss of RuntimeRoot objects or path identity;
- no filename rewriting to satisfy Windows;
- exact image identity verification before use;
- no hidden extraction fallback;
- reproducible build/reader versions or pinned tool identities;
- loader/audit parity with the directory backend;
- useful one-comment/step-summary diagnostics followed by real failure;
- measured restore/startup improvement substantial enough to justify the added reader dependency; and
- no regression to public synthetic, Core, Threaded, generated-route, pthread, descriptor, Mach, socket, VM, or framework-loader behavior.

Possible later optimizations after correctness is established include block caching, dependency-closure prefetch, or an ipaSim-specific immutable RuntimeRoot image format. Do not optimize by dropping RuntimeRoot content or changing guest-visible names.

The WIM experiment remains useful evidence but is not a production fallback: a valid WIM that still requires projection into Windows pathname semantics does not solve the underlying namespace problem.

## Priority 3 — Darwin event delivery: kqueue, kevent, workqueue, and workloop

The current workqueue bridge intentionally does not advertise KEVENT, WORKLOOP, or cooperative delivery modes, and the workloop bridge currently models control-plane lifecycle without full event delivery.

Treat the missing functionality as one eventing subsystem rather than a collection of unrelated exports.

Target capabilities:

- guest-visible kqueue/workloop identity and lifetime;
- event registration and deregistration with exact structure/flag validation;
- readiness delivery for the first defensible Windows-backed event classes;
- integration with libdispatch workqueue callbacks;
- workloop/kevent callback delivery on independent guest-thread execution contexts;
- timeout and wake semantics that do not fabricate readiness;
- explicit rejection of unsupported filters or event classes; and
- deterministic synthetic tests for registration, delivery, cancellation, timeout, and close races.

Do not advertise a workqueue feature bit merely because an export exists.

## Priority 4 — richer Mach IPC and voucher transport

The existing Mach IPC core provides in-process ports, rights counts, bounded queues, inline messages, and timeout behavior. Future compatibility requires expanding that model without turning Mach messages into opaque byte pipes.

Target capabilities:

- complex Mach message descriptors;
- send/receive right transfer with coherent namespace ownership;
- reply-right transfer;
- voucher-port transport through Mach messages;
- descriptor validation and failure codes before state mutation;
- receive-large/scatter behavior where required;
- message priority/QoS handling only when a defensible guest scheduling model exists; and
- port lifetime notifications when real runtime evidence requires them.

The existing voucher callback bridge and Mach message transport remain separate concepts: registering voucher callbacks is not equivalent to transporting voucher rights correctly.

## Priority 5 — complete pthread teardown and synchronization semantics

Core pthread lifecycle is merged; the remaining work should build on that real guest-thread identity model rather than creating parallel thread abstractions.

### TSD teardown

Implement guest-aware pthread-specific-data destructor execution.

Required behavior includes:

- destructor invocation on guest pthread exit;
- Darwin-compatible repeated destructor passes when destructors repopulate keys;
- key deletion/lifetime correctness;
- no x64 execution of guest ARM64 callback addresses; and
- deterministic teardown tests.

### Cancellation

Build a real guest pthread cancellation model before accepting cancellation-point flags in unrelated APIs.

Expected integration points include:

- per-thread cancellation state;
- deferred cancellation points;
- blocking waits;
- cleanup/teardown interaction; and
- cancellation-safe state transitions.

### Ulock thread targeting and ownership

Extend ulock semantics only when Mach thread identity can select a real guest pthread.

Future work includes:

- targeted wake by Mach thread-port identity;
- owner validation tied to the actual guest thread namespace; and
- priority-inheritance/turnstile behavior only where it can be represented honestly on the Windows-backed scheduler model.

Do not infer Darwin turnstile state from unrelated Windows thread priority data.

## Priority 6 — coherent Darwin signal subsystem

Do not implement individual waiting functions as isolated success shims.

A useful signal subsystem should define together:

- guest signal dispositions;
- per-thread signal masks;
- pending process/thread signals;
- mask updates;
- wait/interruption semantics;
- pthread cancellation interaction where Darwin defines a cancellation point; and
- correct restart/interruption behavior for supported blocking APIs.

Until those semantics exist, unsupported signal waits should fail explicitly rather than pretending a signal transition occurred.

## Priority 7 — commpage and CPU capability truth gate

Guest userspace may select code paths based on Apple CPU capability state. ipaSim must never advertise capabilities that the actual ARM64 execution engine cannot execute.

Core rule:

> Never advertise an Apple CPU capability unless the execution engine has a passing executable proof for it.

The future capability model should distinguish at least:

- generic AArch64 baseline instructions;
- architectural atomics/LSE families;
- pointer-authentication instructions and keys;
- BTI behavior;
- Apple-specific system-register expectations;
- AMX or other Apple accelerator instructions; and
- cache-line and CPU-topology properties exposed to userspace.

A capability should be generated/data-driven where useful, but its final advertisement gate must be backed by executable evidence rather than inferred from the host CPU or a nominal Apple device model.

## Priority 8 — filesystem, descriptor, process, and networking detail

The descriptor namespace is substantially more coherent, but future runtime evidence may require broader BSD/XNU behavior.

Likely subsystem work includes:

- additional `fcntl` commands with explicit Darwin-to-Windows translation;
- guarded descriptor capabilities beyond the currently supported guard set;
- vnode/path metadata and additional `proc_pidinfo` flavors;
- relative-path operations that consume the existing per-thread working-directory override correctly;
- filesystem metadata translation with exact Darwin field widths and timestamps; and
- additional socket receive/send variants where address-result and ancillary-data semantics can be represented faithfully.

Variadic, callback-bearing, or untyped SDK interfaces remain outside generated production routing until their callable ABI is independently established.

## Priority 9 — Objective-C, Swift, framework, and higher runtime lifecycle

Once lower Darwin/runtime prerequisites are stable enough to carry modern libSystem/libdispatch behavior, expand higher-level compatibility from public framework fixtures.

Potential boundaries include:

- Objective-C runtime lifecycle and metadata expectations not already handled by the historical bridge;
- blocks/callback ownership and escaping callback lifetimes;
- Swift runtime dependencies;
- Foundation lifecycle and run-loop behavior;
- UIKit/application lifecycle only when lower runtime prerequisites are real;
- graphics/event delivery;
- media/framework services; and
- XPC/service integration.

Higher framework progress must not be achieved by hiding a lower-level Darwin failure.

## Ongoing parallel track — generated semantic-route scaling

The RuntimeRoot priorities above do not suspend safe SDK-wide mechanical work.

When authoritative SDK/compiler evidence and an already-correct semantic provider exist, coherent generated-route migrations may continue in parallel provided they do not overlap an active runtime claim.

Scaling rules remain:

- generator-owned ABI records are the mechanical source of truth;
- explicit semantic-provider approval is separate;
- provider module/export/address identity is checked at runtime;
- pointer-bearing APIs require complete guest-span validation;
- data exports remain data;
- variadics, callbacks, no-prototype declarations, and unresolved stack/carrier classes stay explicit; and
- the handwritten Darwin ABI table should shrink only when a complete generated replacement is actually proven.

Mechanical coverage should not wait for one application to encounter every safe SDK symbol. Semantic implementation still must not be inferred from mechanical coverage.

## Explicit non-goals inherited from full-system emulators

Full-system XNU emulators are useful behavioral references, but many of their patches exist because they boot a complete Apple kernel and root filesystem. Those patches are **not** ipaSim implementation requirements.

Unless ipaSim's architecture changes to boot XNU, do not add compatibility work whose sole purpose is:

- APFS root authentication, snapshot, or root-filesystem patching;
- AMFI/code-signing/trust-cache bypasses;
- kernel `pmap` code-sign enforcement bypasses;
- IMG4/firmware-signature bypasses;
- SEP firmware emulation or activation bypasses;
- Apple interrupt-controller, DART, SART, or other SoC-device emulation; or
- jailbreak-style privilege or mount changes.

ipaSim should model the userspace contract it actually exposes, not reproduce security bypasses needed by a full-system virtual machine.

## Regression contract for every roadmap increment

A roadmap item is not complete merely because the next application failure moves.

Every compatibility PR should preserve the known-good public boundaries and include the smallest focused proof for its new behavior.

Minimum acceptance rules:

- no application-specific names, bundle identifiers, local paths, fingerprints, private logs, screenshots, or private RuntimeRoot contents in public history;
- no monkey patching, runtime swapping, or hidden compatibility hooks;
- no fabricated success for unsupported Darwin behavior;
- complete guest pointer/span validation before host dereference when the API requires it;
- exact Darwin LP64 structure widths and return/error conventions;
- prior generated semantic routes remain green;
- prior descriptor, Mach IPC, pthread/workqueue, VM, timing, socket, filesystem, loader, and RuntimeRootStore smokes remain green when applicable;
- `HelloBootstrap.ipa` continues to execute its known ARM64 result;
- untouched native/UIKit synthetic fixtures continue to reach their documented boundaries until those boundaries are intentionally replaced by real implementation;
- the pinned public third-party workload preserves its documented public boundary until real implementation advances it; and
- CI publishes an actionable failure diagnostic and then fails normally rather than suppressing compiler, linker, packaging, semantic-test, or runtime failure.

## How the next implementation is selected

The roadmap is ordered by architectural dependency and expected modern-runtime pressure, but it does **not** override runtime evidence.

After each independently correct increment merges:

1. start from updated `master`;
2. inspect active claims and open/draft PRs;
3. run the applicable public validation in the order defined by `AGENTS.md`;
4. use trusted full-RuntimeRoot or private/local acceptance only after public baselines are understood;
5. identify the first genuine non-cascading semantic/runtime boundary;
6. map that boundary onto the smallest coherent subsystem above;
7. create a narrow claim;
8. implement the smallest complete subsystem increment that fixes the abstraction rather than only the observed symbol;
9. add public semantic regression coverage;
10. merge when independently correct and green; and
11. repeat from the newly updated `master`.

This preserves the project's central goal: progressively broader modern iOS compatibility on Windows without turning the emulator into a collection of target-specific exceptions.
