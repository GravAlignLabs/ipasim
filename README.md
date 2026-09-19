# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). Jan Joneš's original research remains the foundation of the project. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while keeping unsupported behavior explicit, target-neutral, and diagnosable.

[![Synthetic iOS IPA on Windows](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml)
[![Windows ARM64 Core](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml)
[![Threaded ARM64 Guest Context](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml)

## Project north star

ipaSim is **not** being developed as a sequence of application-specific symbol fixes.

The long-term objective is to derive the **mechanical iOS compatibility surface from Apple SDK metadata and compiler evidence in bulk**, generate reusable ARM64-to-Win64 adapter records, and maintain a separate, explicitly validated semantic-provider catalog. Real application execution is primarily validation and dynamic/behavioral evidence; it is not the primary discovery mechanism for mechanically describable SDK APIs.

```text
                    Apple iOS SDK
                         |
            +------------+------------+
            |                         |
            v                         v
      TAPI export/provider       Clang header/type
          universe                  universe
            |                         |
            +------------+------------+
                         |
                         v
                SDK-wide typed catalog
                         |
                         v
              AAPCS64 mechanical lowering
                         |
                         v
               Win64 carrier lowering
                         |
                         v
                 libffi bridge plans
                         |
                         v
              generated runtime adapters
                         |
              +----------+----------+
              |                     |
              v                     v
     semantic-provider        explicit unsupported /
       approval catalog       complex semantic work
              |
              v
       controlled loader routing
              |
              v
         ARM64 guest execution
```

The SDK/compiler side may establish **how** a function is represented and called. It must never decide by itself that a Windows implementation has the correct Darwin/iOS semantics. Semantic approval remains explicit and fail-closed.

## New-chat / contributor handoff

If you are picking this project up in a new AI chat or as a new contributor, **start here**, then read [`AGENTS.md`](AGENTS.md), [`ROADMAP.md`](ROADMAP.md), and the active coordination claims under [`.github/agent-work/`](.github/agent-work/). Before editing `src/IpaSimulator/`, also read [`src/IpaSimulator/AGENTS.md`](src/IpaSimulator/AGENTS.md).

Treat this README as the current merged-runtime handoff. `ROADMAP.md` is a dependency-oriented subsystem backlog rather than a fixed execution order; the first genuine non-cascading runtime failure remains authoritative even when older roadmap checkpoint text has not yet been refreshed.

## Current merged checkpoint — September 19, 2026

**Latest merged runtime checkpoint: [PR #83 — Implement Mach voucher creation semantics](https://github.com/GravAlignLabs/ipasim/pull/83).**

PR #83 merged on September 8, 2026. The current `master` head after the corresponding green Windows tester snapshot is `eeab11f8781e4cf1911ab2820b77c20fba539156`.

The current evidence-driven progression is:

- [PR #58](https://github.com/GravAlignLabs/ipasim/pull/58) — complete pinned `iPhoneOS16.5.sdk` mechanical compatibility preflight;
- [PR #59](https://github.com/GravAlignLabs/ipasim/pull/59) — deterministic semantic-migration planning from real host exports plus generated adapters;
- [PR #60](https://github.com/GravAlignLabs/ipasim/pull/60) — generic live ARM64 state capture/commit for generated `AdapterRecord` execution;
- [PR #61](https://github.com/GravAlignLabs/ipasim/pull/61) — process identity migration: `_getpid`, `_getuid`, `_geteuid`, `_getgid`, `_getegid`;
- [PR #62](https://github.com/GravAlignLabs/ipasim/pull/62) — scalar descriptor migration: `_close`, `_lseek`;
- [PR #63](https://github.com/GravAlignLabs/ipasim/pull/63) — pointer/positional I/O migration: `_write`, `_pread`, `_pwrite`, with complete guest-span validation;
- [PR #64](https://github.com/GravAlignLabs/ipasim/pull/64) — real socket receive semantics plus generated `_read` routing;
- [PR #65](https://github.com/GravAlignLabs/ipasim/pull/65) — XNU guarded regular-file descriptor semantics;
- [PR #66](https://github.com/GravAlignLabs/ipasim/pull/66) — subsystem-oriented runtime roadmap;
- [PR #67](https://github.com/GravAlignLabs/ipasim/pull/67) — real host-backed Darwin pthread core with independent ARM64 guest execution contexts;
- [PR #68](https://github.com/GravAlignLabs/ipasim/pull/68) — pinned public third-party AWS Device Farm IPA acceptance;
- [PR #69](https://github.com/GravAlignLabs/ipasim/pull/69) — GitHub-hosted iOS simulator RuntimeRoot discovery for trusted acceptance;
- [PR #70](https://github.com/GravAlignLabs/ipasim/pull/70) — direct `tar + zstd -1` RuntimeRoot cache;
- [PR #73](https://github.com/GravAlignLabs/ipasim/pull/73) — correct cross-OS RuntimeRoot cache identity;
- [PR #74](https://github.com/GravAlignLabs/ipasim/pull/74) — isolated read-only WIM RuntimeRoot experiment;
- [PR #75](https://github.com/GravAlignLabs/ipasim/pull/75) — keep WIM diagnostic publishing secondary to the real acceptance result;
- [PR #76](https://github.com/GravAlignLabs/ipasim/pull/76) — introduce the immutable `RuntimeRootStore` byte-source boundary;
- [PR #77](https://github.com/GravAlignLabs/ipasim/pull/77) — prove an NTFS-unrepresentable Darwin pathname can be read directly from DwarFS on Windows;
- [PR #78](https://github.com/GravAlignLabs/ipasim/pull/78) — run the complete pinned iOS 18.5 RuntimeRoot directly from one DwarFS image on Windows;
- [PR #79](https://github.com/GravAlignLabs/ipasim/pull/79) — move static dependency closure and host-import preflights onto the same configured `RuntimeRootStore` used by the loader;
- [PR #80](https://github.com/GravAlignLabs/ipasim/pull/80) — promote the validated Windows DwarFS reader package into the repository so acceptance no longer rebuilds it on cache misses;
- [PR #81](https://github.com/GravAlignLabs/ipasim/pull/81) — implement Darwin `mach_absolute_time` with a real Windows-backed monotonic uptime source; and
- [PR #83](https://github.com/GravAlignLabs/ipasim/pull/83) — implement typed Mach host identity and `host_create_mach_voucher` semantics, while folding in the one-glance RuntimeRoot boundary reporting developed in PR #82.

[PR #82](https://github.com/GravAlignLabs/ipasim/pull/82) was closed without a separate merge because its `NEXT_BOUNDARY` diagnostic work was incorporated into PR #83.

### Current state at a glance

| Area | Current merged state |
| --- | --- |
| Complete RuntimeRoot storage | One verified DwarFS image, read directly on Windows without mounting or extracting the RuntimeRoot tree |
| RuntimeRoot preflights | Static closure, host-import inventory, and loader all use the same explicitly configured `RuntimeRootStore` |
| DwarFS reader | Pinned validated Windows reader package is committed under `deps/dwarfs-reader/`; acceptance does not silently rebuild it |
| Timing | `mach_absolute_time` is implemented with `QueryUnbiasedInterruptTimePrecise`; `mach_continuous_time` retains its separate continuous-time path |
| Mach host identity | `mach_host_self` returns a stable typed host port in the shared Mach namespace |
| Mach vouchers | `host_create_mach_voucher` validates XNU-style recipe/copyout behavior and implements manager-independent `COPY` / `REMOVE` semantics |
| CI diagnostics | The persistent RuntimeRoot PR diagnostic and Actions summary surface a canonical `NEXT_BOUNDARY:` line while preserving real failure behavior |
| Current next boundary | `_host_get_special_port` while applying chained fixups for `/usr/lib/system/libdispatch.dylib` |

## Immediate objective: the `_host_get_special_port` Mach boundary

PR #83's exact-head RuntimeRoot acceptance advanced beyond `_mach_absolute_time` and `_host_create_mach_voucher` to the next genuine loader stop:

```text
Error: symbol _host_get_special_port was not found for library ordinal 3.
Error: cannot apply chained fixups for /usr/lib/system/libdispatch.dylib:
cannot resolve chained-fixup import _host_get_special_port from library ordinal 3.
```

Full RuntimeRoot DwarFS storage acceptance passed on the PR #83 merge-test commit `f14f4c5be53b1e21272f0bc3ae9487b3531ff390` before reporting that boundary. The complete pinned iOS 18.5 (22F77) RuntimeRoot remained one image; the same configured store fed static closure, host-import inventory, and the real loader.

The next semantic increment should therefore investigate `_host_get_special_port` as part of the **Mach host/special-port model**, not add an application-specific alias or success stub. If the correct XNU semantics require a larger coherent host-port abstraction, implement that abstraction and keep unsupported special-port behavior explicit.

Do not pre-implement unrelated later Mach APIs merely because they are nearby. Runtime evidence selects the next semantic boundary.

## RuntimeRoot architecture

The complete RuntimeRoot path is now:

```text
GitHub-hosted macOS runner
        |
        |  Xcode 16.4
        |  iOS simulator runtime 18.5 (22F77)
        v
complete RuntimeRoot
        |
        v
     mkdwarfs
        |
        v
 RuntimeRoot.dwarfs
      ONE FILE
        |
        v
cross-OS Actions cache
        |
        v
   real Windows runner
        |
        +-> exact-head ipaSim Core tester
        +-> pinned validated DwarFS reader bridge
        +-> pinned public AWS IPA
        |
        v
    RuntimeRootStore
        |
        +-> static dependency closure
        +-> host-import inventory
        +-> DynamicLoader
        |
        v
 direct immutable reads from the DwarFS image
```

The intended path has **no RuntimeRoot mount, full extraction, filename sanitization, path exclusion, path rewriting, or NTFS materialization fallback**.

### What PRs #78-#80 proved

PR #78 crossed the storage boundary on real Windows:

- public Windows ARM64 Core, Synthetic IPA, and Threaded ARM64 checks passed;
- the Darwin-only illegal-name DwarFS fixture passed;
- the Windows in-image reader smoke passed;
- the complete pinned RuntimeRoot was restored as one verified **7,202,038,273-byte** DwarFS image; and
- the real loader reached a genuine semantic boundary rather than an image/store failure.

An exact attempt to materialize the complete trusted tar/zstd RuntimeRoot as NTFS stopped before ipaSim with **15,339 hard-link creation errors** and **75 rejected link-path errors**. That host-filesystem limitation is the reason the image-backed architecture exists; it must not be treated as a DwarFS regression or worked around by renaming/dropping Darwin paths.

PR #79 removed the remaining directory-only read detour. Static Mach-O closure and host-import inventory now consume the same explicitly selected `RuntimeRootStore` instance as the loader. The image-backed path therefore no longer skips those preflights.

PR #80 made the already validated DwarFS reader durable without adding Apple RuntimeRoot bytes to Git. The unchanged validated reader ZIP is stored under `deps/dwarfs-reader/windows-x64/` with source fingerprints, binary checksums, provenance, ABI documentation, and third-party notices. CI validates that package and compiles only the small current-source consumer smoke. Missing/corrupt packages or changed build inputs fail explicitly; there is no silent DwarFS/vcpkg rebuild fallback.

The multi-gigabyte RuntimeRoot image remains derived from the pinned GitHub-hosted Xcode runtime and is not committed to or redistributed from this repository.

### Historical directory transport baseline

The earlier trusted transport remains useful historical evidence:

```text
GitHub-hosted macOS RuntimeRoot
        -> complete tar stream
        -> zstd -1 -T0
        -> RuntimeRoot.tar.zst
        -> repository-relative cross-OS Actions cache
        -> Windows restore + SHA-256 verification
        -> full extraction to a Windows directory
        -> Test-Ipa.cmd
```

The measured iOS 18.5 RuntimeRoot contained roughly **467,540 entries** and **19,398,480 KiB** of logical content. PR #70 measured a `zstd -1` package of `9,085,521,586` bytes with about `142` seconds of packaging time on the profiled GitHub macOS runner.

The tar/zstd workflow remains historical transport evidence, not a full-namespace parity oracle, because NTFS cannot reproduce the complete Darwin namespace/link topology exactly.

PR #74 also tested a WIM-based route. The WIM itself was valid, but real Windows DISM mounting failed with **Error 123** around 79% progress. No RuntimeRoot objects were excluded or renamed to force success. That experiment reinforced the architectural requirement: Windows should not have to project Apple's complete RuntimeRoot as ordinary Windows paths before ipaSim can consume it.

## Current Mach timing and voucher state

### Darwin timing

PR #81 implements `mach_absolute_time()` through Windows `QueryUnbiasedInterruptTimePrecise`, preserving monotonic uptime while excluding system sleep. The existing 100 ns Mach tick representation and `mach_timebase_info` value of `100/1` remain coherent with that source.

`mach_continuous_time` remains a separate continuous-time path backed by `QueryInterruptTimePrecise`.

The exact zero-argument, 64-bit ARM64 host-call ABI is registered explicitly; timing semantics are validated in the focused Darwin time smoke rather than treated as a trivial alias.

### Mach host and voucher semantics

PR #83 extends the shared Mach namespace with typed host and voucher objects:

- `mach_host_self()` returns a stable host port name;
- unrelated task/message/voucher names cannot masquerade as a host capability;
- `host_create_mach_voucher()` validates the complete guest recipe/output spans before host dereference;
- Darwin's packed 16-byte voucher recipe ABI and 5120-byte raw recipe limit are enforced;
- manager-independent `COPY` and `REMOVE` recipe behavior is implemented;
- `previous_voucher` capabilities are validated against the typed Mach namespace;
- manager-specific attribute behavior fails with `KERN_NOT_SUPPORTED` instead of fabricated success;
- invalid host conversion, malformed input, oversized recipes, copyin/copyout faults, and failed calls preserve XNU-style error/copyout layering; and
- the exact XNU zero-recipe behavior is preserved: a valid host plus an empty recipe returns success with `MACH_VOUCHER_NULL` rather than manufacturing a voucher object.

The PR intentionally did **not** pre-implement `host_get_special_port`, `mach_port_deallocate`, voucher attribute managers, or later voucher transport/extraction APIs. The subsequent RuntimeRoot acceptance is what selected `_host_get_special_port` as the next boundary.

## Public acceptance and frozen regression contracts

Contributors do not need a private application to reproduce core compatibility work.

### Synthetic fixtures

- **`HelloBootstrap.ipa`** — minimal loader/Unicorn proof; successful guest execution returns `X0=42`.
- **`HelloNative.ipa`** — untouched minimal iOS executable used to expose the next Apple runtime boundary.
- **`HelloUIKit.ipa`** — small UIKit/Foundation application used to expose framework/runtime boundaries publicly.

Expected boundary diagnostics in these fixtures are evidence, not noise. Do not suppress an `Error:` line merely because the surrounding workflow is intentionally validating that boundary.

### Pinned public third-party IPA

PR #68 pins the Apache-2.0 AWS Device Farm sample IPA at upstream commit `58e48234db510bd4fbf643643e8808c5d6a13845` and Git blob `06a33a39286ffd7c9d300c5924750b6f97c4e346`. CI downloads and verifies the upstream object instead of committing the IPA into this repository.

The no-RuntimeRoot public regression contract remains intentionally:

```text
probe_exit=2
first_error=Error: unsupported ARM64 relocation.
runtime_boundary=Error: iOS runtime root is not configured for dependency /System/Library/Frameworks/AVFoundation.framework/AVFoundation.
loader_stop=[ipasim-probe] loader stopped with code 2 before app execution.
```

The relocation diagnostic must remain visible until real target-neutral relocation support replaces it. A later dependency error is not permission to hide an earlier failure.

Trusted full-RuntimeRoot DwarFS acceptance is a stronger layer on top of the public baselines. Its purpose is to expose the next genuine loader/runtime/framework boundary using the complete pinned RuntimeRoot; it must not weaken the public contracts merely to advance farther.

## CI self-reporting contract

Build, compiler, linker, packaging, reader, loader, and runtime failures must remain visible.

For the Windows core and RuntimeRoot acceptance paths:

- capture the real diagnostic output;
- preserve the real failing exit code;
- publish the useful actionable failure to the Actions step summary;
- create or update **one persistent PR diagnostic comment** instead of creating duplicates;
- keep the comment updated on later pushes;
- surface the canonical RuntimeRoot boundary directly as `NEXT_BOUNDARY:` when acceptance successfully reaches a later genuine compatibility stop; and
- after publishing diagnostics, fail CI normally when the underlying step actually failed.

Diagnostic publishing is secondary to the real build/test. It must never suppress a failure, manufacture success, or block a real acceptance job from running.

## Current generated production routes

There are currently **11 explicitly approved generated production routes**:

```text
_close
_getegid
_geteuid
_getgid
_getpid
_getuid
_lseek
_pread
_pwrite
_read
_write
```

The production route is:

```text
real Mach-O import resolution
        -> normal PE export resolution
        -> explicit semantic-approval table lookup
        -> exact provider module/export/address verification
        -> generated AdapterRecord requirements
        -> live AAPCS64 GPR/SIMD/stack capture as required
        -> generated pointer gate when required
        -> real semantic provider
        -> provider-level complete-span / API-specific validation
        -> generated result/state commit
        -> ARM64 guest execution resumes
```

Generated SDK/ABI evidence determines call mechanics only. It never grants semantic implementation approval.

The 11-route count is unchanged by the newer timing and Mach-voucher work. APIs such as `mach_absolute_time`, `mach_host_self`, and `host_create_mach_voucher` use target-proven explicit ABI/semantic integration at their current subsystem boundary rather than being silently counted as approved SDK-generated routes.

`guarded_open_np` and `guarded_close_np` likewise remain intentionally outside the generated adapter table because the pinned public SDK evidence does not provide a generator-owned typed callable ABI for those exports. Their ABI/behavior is backed by authoritative XNU/libdispatch evidence instead of being guessed into the generated route set.

## Current Darwin pthread state

PR #67 merged the core guest pthread lifecycle onto the current runtime:

- `pthread_create` uses a real Windows backing thread and a fresh ARM64 Unicorn execution context;
- worker callbacks retain their own `SysTranslator` instead of falling back to the process-global main engine;
- guest-visible `pthread_t` identity remains separate from Windows thread handles;
- join/detach/exit state is serialized and ownership races fail explicitly;
- guest `pthread_exit` unwinds logical guest execution instead of terminating the host process;
- Darwin LP64 widths and Darwin-specific errno values remain explicit where Win64 differs;
- active guest stacks expose truthful bounds; fake guest stack addresses are not manufactured; and
- unsupported custom stacks and fixed scheduling remain fail-closed.

Darwin threading is not finished. TSD destructor teardown, cancellation, real signal delivery, kevent/workloop event delivery, richer Mach thread-port identity, custom guest stacks, and stronger scheduling semantics remain later subsystem work when evidence requires them.

## SDK-wide mechanical compatibility engine

The modern compatibility engine is deliberately broader than any one application.

PR #58 exercises the complete pipeline against `theos/sdks@0222fd5413cf4b9af096f37b4621afa2688572f7`, scoped to `iPhoneOS16.5.sdk`.

The successful mechanical coverage snapshot is:

- physical headers analyzed: **5,118 / 5,118**
- TAPI symbols: **1,355,229**
- Clang header C signatures: **13,795**
- SDK catalog symbols: **1,354,457**
- typed C candidates: **13,298**
- AAPCS64 generated candidates: **12,599**
- Win64 cross-ABI candidates: **12,515**
- generated runtime adapters: **10,599**
- explicitly approved generated production routes: **11**

```text
pinned iPhoneOS16.5.sdk
        -> complete TAPI scan                    PASS
        -> exhaustive Clang header indexing      PASS: 5,118/5,118
        -> SDK typed catalog                     PASS
        -> AAPCS64 lowering                      PASS
        -> Win64 carrier lowering                PASS
        -> libffi bridge plans                   PASS
        -> generated runtime adapters            PASS: 10,599
        -> compatibility planner                 PASS
        -> semantic-route comparison             explicit approval boundary
        -> generic live generated execution      PASS
        -> approved production routes            11
        -> real semantic/runtime boundaries      evidence-driven, fail-closed
```

Passing this preflight does **not** mean 10,599 iOS APIs are semantically implemented on Windows. It means the mechanical SDK/compiler pipeline can describe and carry that proven subset without weakening validation.

Core mechanical rules:

- scan SDK metadata in bulk rather than waiting for one app to import each function;
- use TAPI metadata for direct exports and explicit re-export relationships;
- use Clang for SDK header signatures and compiler-lowered ABI evidence;
- preserve Apple LP64 carrier widths across the Win64 LLP64 host boundary;
- let libffi own proven host call mechanics where appropriate;
- keep guest pointers opaque until complete runtime validation;
- keep callbacks, variadics, no-prototype declarations, unresolved stack placement, Objective-C metadata, TLS, and data exports explicit rather than guessing;
- require explicit semantic ownership before a generated adapter may call a real provider; and
- reject missing, non-executable, or data exports instead of treating export presence as compatibility.

## Compatibility subsystems implemented so far

### ARM64 execution and loading

- real ARM64/AArch64 Unicorn execution on Windows x64;
- AAPCS64 register handling and pointer-width host-call translation;
- modern ARM64 Mach-O parsing and image loading;
- chained fixups and exports-trie resolution;
- dependency ordinal handling and RuntimeRoot resolution;
- shared-memory multi-engine execution;
- independent secondary guest execution contexts and threaded callbacks;
- controlled loader selection of explicitly approved generated semantic routes;
- generated live GPR/SIMD/stack capture and result commit driven by `AdapterRecord`;
- immutable RuntimeRoot byte-source abstraction with explicit directory and DwarFS backends;
- direct complete RuntimeRoot image reads on Windows with no mount/extraction fallback; and
- store-backed static dependency and host-import preflights.

### Darwin/runtime work

Implemented coverage includes process identity and selected process information, Mach task/time/VM behavior, typed host/voucher identity, initial Mach voucher creation semantics, ulock synchronization, libplatform memory/string primitives, a coherent guest-visible file-descriptor namespace, regular files/FIFOs, Darwin ARM64 `stat` translation, socket/WinSock send and receive translation, generated scalar and pointer-bearing descriptor I/O, XNU guarded regular-file descriptor semantics, pthread core/QoS/TSD/workloop/workqueue control-plane work, and independent guest worker execution.

Unsupported behavior remains explicit rather than being fabricated as success.

## How the next implementation is selected

Use this sequence after every independently correct checkpoint merges:

1. update to current `master`;
2. confirm any PR you were working on has not already merged before continuing it;
3. inspect `.github/agent-work/` and open/draft PRs for overlapping work;
4. run the applicable public validation workflows;
5. run the pinned public acceptance workload through the strongest trusted RuntimeRoot path available;
6. use private/local application execution only as additional evidence and never expose identifying data publicly;
7. identify the **first genuine non-cascading semantic/runtime failure**;
8. map it to the smallest coherent subsystem rather than treating one symbol as the architecture;
9. create a narrow coordination claim when required;
10. implement target-neutral behavior with explicit failure for unsupported semantics;
11. add or preserve public regression proof; and
12. delete the claim from the implementation PR before merge.

A missing symbol is evidence of a boundary, not automatically the unit of implementation.

## Public validation order

Follow [`AGENTS.md`](AGENTS.md) for the authoritative order. The normal public regression sequence is:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. **Threaded ARM64 Guest Context** when guest-thread/callback execution is relevant
4. **Compatibility Surface Analyzer** when generated compatibility tooling or fixtures change
5. **RuntimeRoot DwarFS Reader and Acceptance** when RuntimeRoot/store/loader or current semantic boundary files change
6. optional local/private acceptance only after the public results are understood

Additional public/trusted acceptance workflows may exercise the pinned AWS workload and GitHub-hosted RuntimeRoot. Those layers complement the core regression sequence; they do not replace it.

## Regression and privacy rules

Regression prevention is a hard acceptance criterion.

- no monkey patching, runtime swapping, or hidden compatibility hooks;
- no application-specific names, bundle identifiers, local paths, fingerprints, private logs, or private RuntimeRoot data in public history;
- no fabricated success for unsupported Darwin behavior;
- no RuntimeRoot object exclusions, filename rewriting, or extraction fallback used to make Windows accept an incompatible path;
- complete guest pointer/span validation before host dereference when an API requires it;
- exact Darwin LP64 structure widths and return/error conventions;
- generated ABI evidence determines mechanics only;
- semantic approval remains separate and explicit;
- data exports remain data;
- prior generated routes and semantic smokes remain green;
- existing descriptor, Mach IPC, pthread/workqueue, VM, timing, socket, filesystem, loader, RuntimeRootStore, DwarFS-reader, and public-acceptance behavior remain regression requirements when applicable; and
- public synthetic and AWS acceptance boundaries remain frozen until a real implementation intentionally advances them.

## Apple SDK metadata for compatibility research

Use the maintained [Theos SDK archive](https://github.com/theos/sdks) as the pinned public provider/header reference for the established iPhoneOS16.5 mechanical preflight.

GitHub's macOS runner also exposes real `iphoneos` and `iphonesimulator` SDK installations. Trusted RuntimeRoot work currently uses the GitHub-hosted Xcode 16.4 / iOS 18.5 environment as runtime source evidence. Do not silently replace the established pinned Theos mechanical pipeline with a different SDK source without a separately claimed, regression-proven migration.

`.tbd` files are useful for exported names, install names, provider/re-export relationships, targets, weak/TLS/Objective-C metadata classes, and SDK-version comparisons. They are **not implementation source**. Function prototypes require SDK headers/compiler evidence, and correct Windows semantics require independent implementation evidence.

See [`LOCAL_THEOS_PREFLIGHT.md`](LOCAL_THEOS_PREFLIGHT.md) for the resumable Windows/WSL full-SDK pipeline.

## AI coding agents

[`AGENTS.md`](AGENTS.md) is the canonical instruction set for autonomous and AI-assisted work. Changes below `src/IpaSimulator/` also follow [`src/IpaSimulator/AGENTS.md`](src/IpaSimulator/AGENTS.md).

Before starting substantial work:

1. refresh your view of `master`;
2. verify any prior/current PR is still open before continuing it;
3. inspect active claims and open/draft PRs;
4. publish a narrow non-overlapping claim when required;
5. keep implementation target-neutral and evidence-driven;
6. preserve known-good public contracts; and
7. remove the claim from the implementation PR before merge.

For mechanical compatibility work, first ask whether the required evidence belongs in the SDK-wide catalog/generator rather than a handwritten per-symbol table. For semantic/runtime work, the first genuine non-cascading failure remains the truth source.

For the current merged head, that runtime truth source is `_host_get_special_port` from the full image-backed iOS 18.5 RuntimeRoot acceptance path.

---

# Original ipaSim project

This repository contains source code of `ipasim`, an iOS emulator for Windows. It takes a compiled iOS application and emulates the application's machine code while translating system functionality to equivalent functionality available on Windows. [More detailed historical documentation](docs/README.md) is available.

The original implementation supported simple applications. See the [author's thesis](docs/thesis/README.md), especially its conclusion, for the original project's implemented and unimplemented scope.

## Related projects

- [touchHLE](https://github.com/touchHLE/touchHLE) — high-level iOS emulator with useful subsystem comparison points, although its architecture and target era differ
- [UWin](https://github.com/lujingyu/UWin) — historical 2011 iOS/Xcode build snapshot used only as external research evidence

## Cloning and historical build documentation

The repository uses Git submodules recursively and Git LFS. Clone with `--recurse-submodules` when you need the complete historical dependency tree.

Historical build/install documentation remains under:

- [`docs/build.md`](docs/build.md)
- [`docs/artifacts.md`](docs/artifacts.md)
- [`docs/install.md`](docs/install.md)

The active ARM64 fork is validated primarily through the public GitHub Actions workflows described above.

## Directory structure

- [`deps`](deps) — third-party dependencies, including the pinned DwarFS reader package used by RuntimeRoot acceptance
- [`docs`](docs) — documentation and research material
- [`include`](include) — C++ headers
- [`samples`](samples) — sample applications and fixtures
- [`scripts`](scripts) — build/test scripts
- [`src`](src) — project sources
  - [`HeadersAnalyzer`](src/HeadersAnalyzer/README.md) — original compile-time support-code generator
  - [`IpaSimulator`](src/IpaSimulator/README.md) — emulator
  - [`objc`](src/objc/README.md) — Windows port of Apple's Objective-C runtime

## Original research

[![Poster preview](docs/thesis/poster.png)](docs/thesis/poster.pdf)

- [iOS emulator for Windows](docs/thesis/README.md), a bachelor thesis by [Jan Joneš](https://github.com/jjonescz)
