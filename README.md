# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). Jan Joneš's original research remains the foundation of the project. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while keeping unsupported behavior explicit and diagnosable.

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

### Current merged checkpoint — September 4, 2026

**Latest merged compatibility/storage checkpoint: [PR #78 — Run full RuntimeRoot directly from DwarFS on Windows](https://github.com/GravAlignLabs/ipasim/pull/78).**

The recent progression is:

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
- [PR #73](https://github.com/GravAlignLabs/ipasim/pull/73) — correct cross-OS RuntimeRoot cache identity by using the same repository-relative cache path on macOS and Windows;
- [PR #74](https://github.com/GravAlignLabs/ipasim/pull/74) — isolated read-only WIM RuntimeRoot experiment;
- [PR #75](https://github.com/GravAlignLabs/ipasim/pull/75) — keep auxiliary WIM diagnostic publishing non-blocking while preserving real acceptance failure;
- [PR #76](https://github.com/GravAlignLabs/ipasim/pull/76) — decouple RuntimeRoot loading from Windows file paths with an immutable `RuntimeRootStore` byte-source boundary; and
- [PR #77](https://github.com/GravAlignLabs/ipasim/pull/77) — prove Windows can read an NTFS-unrepresentable Darwin pathname directly from a DwarFS image without mounting, extracting, renaming, or falling back; and
- [PR #78](https://github.com/GravAlignLabs/ipasim/pull/78) — build and verify the complete iOS 18.5 RuntimeRoot as one DwarFS image, then feed the exact-head loader directly from that image on Windows.

PR #78's final head preserved the public Core, Synthetic IPA, Threaded ARM64, DwarFS-reader, full-image, and exact-head loader acceptance checks before merge.

[`ROADMAP.md`](ROADMAP.md) is a **dependency-oriented subsystem backlog**, not an unconditional execution order. **The first genuine non-cascading runtime failure may override the nominal priority numbering.**

## Immediate objective: complete RuntimeRootStore read path

PR #78 proved that the complete pinned GitHub-hosted iOS 18.5 RuntimeRoot can remain one DwarFS image and feed ipaSim's real loader directly on Windows:

```text
GitHub macOS runner
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
        +-> validated DwarFS reader bridge
        +-> pinned public AWS IPA
        |
        v
    DynamicLoader
        |
        v
   RuntimeRootStore
        |
        v
 direct immutable reads from the DwarFS image
```

The intended path has **no RuntimeRoot mount, full extraction, filename sanitization, path exclusion, or NTFS materialization fallback**.

### Merged PR #78 checkpoint

The full-image experiment crossed the storage boundary on real Windows and merged at `f56db5be4fafb7fb54cf79a7c8906500ec275317`:

- **Windows ARM64 Core**, **Synthetic iOS IPA on Windows**, and **Threaded ARM64 Guest Context** all passed;
- the Darwin-only illegal-name DwarFS fixture passed;
- the Windows in-image DwarFS reader smoke passed;
- the complete pinned RuntimeRoot was restored as one verified **7,202,038,273-byte** DwarFS image;
- the exact-head Windows probe loaded real Apple frameworks and dylibs directly from the image without mounting or extracting the RuntimeRoot; and
- the first real loader stop was an unresolved `_mach_absolute_time` import while applying chained fixups for `/usr/lib/system/libsystem_sim_platform.dylib`.

The relevant boundary is:

```text
Error: symbol _mach_absolute_time was not found for library ordinal 5.
Error: cannot apply chained fixups for /usr/lib/system/libsystem_sim_platform.dylib:
cannot resolve chained-fixup import _mach_absolute_time from library ordinal 5.
```

That is a successful **image-backed storage/loader proof** under the Priority 0 roadmap criterion: the complete image-backed path reached a later genuine compatibility boundary than Windows directory materialization can reach. An exact-head attempt to rebuild the complete trusted tar/zstd RuntimeRoot as an NTFS directory stopped before ipaSim with **15,339 hard-link creation errors** and **75 rejected link-path errors**. Treating that host-filesystem limitation as a DwarFS regression would make the acceptance gate impossible by construction.

PR #78 validates the complete DwarFS path directly. A nonzero probe is accepted only when it reaches a real unresolved-symbol plus chained-fixup loader boundary; image identity, reader bridge, open/read, malformed output, or earlier storage failures still publish the actionable diagnostic and fail normally. `_mach_absolute_time` was not implemented inside that storage PR merely to force the image-backed path farther.

The Windows reader job caches the finished validated bridge plus its smoke executable under a key derived from the reader sources, build recipe, DwarFS source identity, vcpkg baseline, MSVC version, OS, and architecture. A cache hit still runs the Darwin-only pathname smoke before the DLL is published for acceptance; stale or incomplete packages fail explicitly.

The existing tar/zstd workflow remains unchanged as the historical directory transport baseline, but it is not used as a full-namespace parity oracle after the NTFS limitation is observed. The active Priority 1 checkpoint moves static closure and host-import inventory reads onto the same configured `RuntimeRootStore` used by the loader. This removes their directory-only detour without adding auto-detection, extraction fallback, or a second DwarFS store instance.

### Why the RuntimeRoot architecture changed

The trusted GitHub-hosted RuntimeRoot is large as a filesystem tree: the measured iOS 18.5 RuntimeRoot contains roughly **467,540 entries** and **19,398,480 KiB** of logical content. PR #70's measured `zstd -1` package was `9,085,521,586` bytes and took about `142` seconds to create on the profiled GitHub macOS runner.

The current trusted baseline is:

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

This remains the frozen historical transport baseline, but the complete archive is now known to contain Darwin names and link topology that Windows cannot reconstruct exactly as an NTFS directory. It remains useful evidence for the earlier workflow history; it is not a valid full-namespace parity oracle for the image-backed store.

PR #74 tested a different idea: preserve the complete RuntimeRoot in one WIM and mount it read-only with DISM. The WIM itself was valid and independently verifiable, but the real Windows DISM mount failed with **Error 123** around 79% progress. No RuntimeRoot paths were excluded or renamed to force success. The result demonstrated that a valid single-file package is not enough if Windows must still project Apple's complete namespace as ordinary Windows paths.

PR #76 therefore moved the architectural boundary **inside ipaSim**:

```text
DynamicLoader
     |
     v
RuntimeRootStore
   /       \
  v         v
Directory   future immutable image source
baseline
```

PR #77 then supplied the first image-backed proof. A synthetic DwarFS fixture contains the Darwin path:

```text
/System/Library/Frameworks/UIKit.framework/Versions/A:/UIKit
```

The `A:` component cannot be materialized as a normal NTFS pathname component, but the Windows reader retrieves the exact bytes directly from the DwarFS image. That proves guest Darwin path identity no longer has to equal host Windows pathname identity.

The permanent direction is therefore: **let ipaSim understand a Darwin RuntimeRoot source directly instead of requiring Windows to pretend the RuntimeRoot is an NTFS tree.**

## Public acceptance and frozen regression contracts

Contributors do not need a private application to reproduce core compatibility work.

### Synthetic fixtures

- **`HelloBootstrap.ipa`** — minimal loader/Unicorn proof; successful guest execution returns `X0=42`.
- **`HelloNative.ipa`** — untouched minimal iOS executable used to expose the next Apple runtime boundary.
- **`HelloUIKit.ipa`** — small UIKit/Foundation application used to expose framework/runtime boundaries publicly.

Expected boundary diagnostics in these fixtures are evidence, not noise. Do not suppress an `Error:` line merely because the surrounding workflow is intentionally validating that boundary.

### Pinned public third-party IPA

PR #68 pins the Apache-2.0 AWS Device Farm sample IPA at upstream commit `58e48234db510bd4fbf643643e8808c5d6a13845` and Git blob `06a33a39286ffd7c9d300c5924750b6f97c4e346`. CI downloads and verifies the upstream object instead of committing the IPA into this repository.

The no-RuntimeRoot public regression contract is intentionally:

```text
probe_exit=2
first_error=Error: unsupported ARM64 relocation.
runtime_boundary=Error: iOS runtime root is not configured for dependency /System/Library/Frameworks/AVFoundation.framework/AVFoundation.
loader_stop=[ipasim-probe] loader stopped with code 2 before app execution.
```

The relocation diagnostic must remain visible until real target-neutral relocation support replaces it. A later dependency error is not permission to hide the earlier failure.

Trusted full-RuntimeRoot acceptance is a stronger layer on top of the public baselines. Its purpose is to expose the next genuine loader/runtime/framework boundary using the complete pinned RuntimeRoot; it must not weaken the public contracts to advance farther.

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

`guarded_open_np` and `guarded_close_np` remain intentionally outside the generated adapter table because the pinned public SDK evidence does not provide a generator-owned typed callable ABI for those exports. Their current ABI and behavior are backed by authoritative XNU/libdispatch evidence instead of being guessed into the generated route set.

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
- generated live GPR/SIMD/stack capture and result commit driven by `AdapterRecord`; and
- immutable RuntimeRoot byte-source abstraction with the existing directory backend preserved as the known-good baseline.

### Darwin/runtime work

Implemented coverage includes process identity and selected process information, Mach task/time/VM behavior, ulock synchronization, libplatform memory/string primitives, a coherent guest-visible file-descriptor namespace, regular files/FIFOs, Darwin ARM64 `stat` translation, implemented socket/WinSock send and receive translation, generated scalar and pointer-bearing descriptor I/O, XNU guarded regular-file descriptor semantics, pthread core/QoS/TSD/workloop/workqueue control-plane work, and independent guest worker execution.

Unsupported behavior remains explicit rather than being fabricated as success.

## How the next implementation is selected

Use this sequence after every independently correct checkpoint merges:

1. update to current `master`;
2. inspect `.github/agent-work/` and open/draft PRs;
3. run the applicable public validation workflows;
4. run the pinned public acceptance workload through the strongest trusted RuntimeRoot path available;
5. use private/local application execution only as additional evidence and never expose identifying data publicly;
6. identify the **first genuine non-cascading semantic/runtime failure**;
7. map it to the smallest coherent subsystem rather than treating one symbol as the architecture;
8. create a narrow coordination claim;
9. implement target-neutral behavior with explicit failure for unsupported semantics;
10. add or preserve public regression proof; and
11. delete the claim from the implementation PR before merge.

A missing symbol is evidence of a boundary, not automatically the unit of implementation.

## Public validation order

Follow [`AGENTS.md`](AGENTS.md) for the authoritative order. The normal public regression sequence is:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. **Threaded ARM64 Guest Context** when guest-thread/callback execution is relevant
4. **Compatibility Surface Analyzer** when generated compatibility tooling or fixtures change
5. optional local/private acceptance only after the public results are understood

Additional public/trusted acceptance workflows may exercise the pinned AWS workload and GitHub-hosted RuntimeRoot. Those layers complement the core regression sequence; they do not replace it.

CI failures should preserve the real compiler/linker/packaging/runtime exit code, publish the most useful diagnostic available, and then fail normally. Auxiliary diagnostic publishing must not convert a real failure into success or prevent the real test from running.

## Regression and privacy rules

Regression prevention is a hard acceptance criterion.

- no monkey patching, runtime swapping, or hidden compatibility hooks;
- no application-specific names, bundle identifiers, local paths, fingerprints, private logs, or private RuntimeRoot data in public history;
- no fabricated success for unsupported Darwin behavior;
- complete guest pointer/span validation before host dereference when an API requires it;
- exact Darwin LP64 structure widths and return/error conventions;
- generated ABI evidence determines mechanics only;
- semantic approval remains separate and explicit;
- data exports remain data;
- prior generated routes and semantic smokes remain green;
- existing descriptor, Mach IPC, pthread/workqueue, VM, timing, socket, filesystem, loader, and RuntimeRootStore behavior remain regression requirements when applicable; and
- public synthetic and AWS acceptance boundaries remain frozen until a real implementation intentionally advances them.

## Apple SDK metadata for compatibility research

Use the maintained [Theos SDK archive](https://github.com/theos/sdks) as the pinned public provider/header reference for the established iPhoneOS16.5 mechanical preflight.

GitHub's macOS runner also exposes real `iphoneos` and `iphonesimulator` SDK installations. The trusted RuntimeRoot work currently uses the GitHub-hosted Xcode 16.4 / iOS 18.5 environment as runtime source evidence. Do not silently replace the established pinned Theos mechanical pipeline with a different SDK source without a separately claimed, regression-proven migration.

`.tbd` files are useful for exported names, install names, provider/re-export relationships, targets, weak/TLS/Objective-C metadata classes, and SDK-version comparisons. They are **not implementation source**. Function prototypes require SDK headers/compiler evidence, and correct Windows semantics require independent implementation evidence.

See [`LOCAL_THEOS_PREFLIGHT.md`](LOCAL_THEOS_PREFLIGHT.md) for the resumable Windows/WSL full-SDK pipeline.

## AI coding agents

[`AGENTS.md`](AGENTS.md) is the canonical instruction set for autonomous and AI-assisted work. Changes below `src/IpaSimulator/` also follow [`src/IpaSimulator/AGENTS.md`](src/IpaSimulator/AGENTS.md).

Before starting substantial work:

1. update your view of `master`;
2. inspect active claims and open/draft PRs;
3. publish a narrow non-overlapping claim when required;
4. keep implementation target-neutral and evidence-driven;
5. preserve known-good public contracts; and
6. remove the claim from the implementation PR before merge.

For mechanical compatibility work, first ask whether the required evidence belongs in the SDK-wide catalog/generator rather than a handwritten per-symbol table. For semantic/runtime work, the first genuine non-cascading failure remains the truth source.

See [`ROADMAP.md`](ROADMAP.md) for the current subsystem backlog and [`docs/arm64-ios-compatibility.md`](docs/arm64-ios-compatibility.md) for additional compatibility direction.

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

- [`deps`](deps) — third-party dependencies
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
