# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). The original project and research remain the foundation of this work. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while preserving explicit diagnostics for behavior that is not yet implemented.

[![Synthetic iOS IPA on Windows](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml)
[![Windows ARM64 Core](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml)
[![Threaded ARM64 Guest Context](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml)

## Project north star

ipaSim is **not** being developed as a sequence of application-specific symbol fixes.

The long-term objective is to derive the **mechanical iOS compatibility surface from Apple SDK metadata and compiler evidence in bulk**, generate reusable ARM64-to-Win64 adapter records, and maintain a separate, explicitly validated semantic-provider catalog. A real IPA is primarily a validation target and a source of dynamic/behavioral evidence; it should not be the primary mechanism for discovering which mechanically describable SDK APIs exist.

The intended scaling model is:

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

The SDK/compiler side may tell ipaSim **how** a function is represented and called. It must never decide by itself that a Windows implementation has the correct Darwin/iOS semantics. That approval boundary remains explicit and fail-closed.

## New-chat / contributor handoff

If you are picking this project up in a new AI chat or as a new contributor, start here and then read [`AGENTS.md`](AGENTS.md) plus the active coordination claims under [`.github/agent-work/`](.github/agent-work/).

**Latest merged compatibility-engine checkpoint: [PR #53 — Generalize generated semantic import routing table](https://github.com/GravAlignLabs/ipasim/pull/53).**

PR #52 proved the first real production route from a loader-resolved ARM64 import to a generated adapter and an explicitly approved semantic provider. `_getpid` now travels through the real loader, generated ARM64/Win64 adapter metadata, the approved `IpaSimDarwinHost.dll!getpid` provider, libffi, and back into ARM64 guest result state instead of relying on the old handwritten mechanical signature entry.

PR #53 removed the `_getpid`-specific routing decision and made semantic route selection table-driven. Generated ABI evidence and semantic approval remain deliberately separate: a generated adapter does not become callable in production merely because the SDK/compiler can describe it.

The generated production path is now:

```text
real Mach-O import resolution
        -> normal PE export resolution
        -> explicit semantic-approval table lookup
        -> generated adapter/profile validation
        -> approved provider module verification
        -> exact live export-address verification
        -> generated adapter / libffi execution
        -> ARM64 guest result state
```

At the PR #53 checkpoint, the applicable public validation paths were green: **Compatibility Surface Analyzer, Windows ARM64 Core, Threaded ARM64 Guest Context, and Synthetic iOS IPA on Windows**. The green Windows tester snapshot is published automatically from the validated core build.

The next scaling boundary is **not** a long series of trivial `_getuid`, `_geteuid`, `_getgid`, and similar one-symbol migration PRs. The compatibility-engine work is moving to an SDK-wide typed catalog so mechanically provable API/ABI coverage can be generated in bulk before any particular application happens to execute those symbols.

There is also a parallel active **Darwin pthread core** work claim. Before changing pthread, `SysTranslator.cpp`, `IpaSimulator.cpp`, `src/IpaSimulator/CMakeLists.txt`, or nearby guest-thread lifecycle code, inspect `.github/agent-work/` and open PRs to avoid overlapping that work.

## Current ARM64 work

The modern ARM64 foundation began with [PR #3: ARM64 + modern dyld foundation for iOS compatibility](https://github.com/GravAlignLabs/ipasim/pull/3). Development has now advanced through [PR #53: Generalize generated semantic import routing table](https://github.com/GravAlignLabs/ipasim/pull/53).

The project is working on **two connected layers at the same time**:

1. continuing the real Windows-backed Darwin/runtime implementation required by modern iOS binaries, and
2. reviving and extending the strongest idea from Jan Joneš's original ipaSim thesis: use SDK/compiler metadata to generate the mechanical ABI bridge instead of hand-writing one symbol signature at a time.

The important refinement is that the second layer is now intended to operate **SDK-wide**, not only on symbols already encountered by one application.

### Verified checkpoint — August 31, 2026

The public runtime validation paths currently prove that the ARM64 emulator core, threaded guest execution model, synthetic iOS loader path, generated ABI pipeline, controlled semantic-provider bridge, and real loader-selected generated route work together:

- **Windows ARM64 Core** builds the x64 Windows emulator core, executes real AArch64 instructions through Unicorn, runs the Darwin subsystem semantic smoke suite, executes generated bridge adapter smokes, and validates the generated semantic-provider/import-routing proof.
- **Threaded ARM64 Guest Context** executes an ARM64 guest function on an independent Windows host thread with its own Unicorn engine, register state, and guest stack, returning the expected `X0=42`.
- **Synthetic iOS IPA on Windows** builds public iOS ARM64 fixtures with Xcode and exercises the Windows ipaSim loader/runtime path against them.
- **Compatibility Surface Analyzer** validates the deterministic importer, SDK, signature, ABI-lowering, bridge-plan, runtime-adapter, semantic-provider fixture, and related generated tooling.

The bridge runtime handles controlled integer, mixed GPR/SIMD, pointer-gated, aggregate, guest-stack, and ARM64 `x8` indirect-result cases through the vendored Win64 libffi backend. Generated adapters are duplicate-safe, require explicit host bindings, validate pointer-sensitive calls before execution, and reject carrier/layout assumptions that are not proven.

This is **not yet a claim that arbitrary modern iOS applications run to completion**. Foundation/UIKit, Objective-C/Swift runtime integration, XPC, graphics, device services, broader Darwin semantics, callbacks, variadics, and several production execution profiles remain active compatibility boundaries.

## Current direction: SDK-wide generated cross-ABI compatibility engine

The original thesis did not scale ipaSim by manually writing every ABI wrapper. Its `HeadersAnalyzer` used SDK metadata and Clang type information to generate mechanical glue. The modern ARM64 fork is rebuilding that idea as a deterministic, testable AAPCS64-to-Win64 compatibility engine, then extending it so the mechanical map can be built from the **whole SDK surface** rather than only from the next runtime failure.

There are now two useful views of the same compatibility knowledge:

```text
SDK-wide planning view
----------------------
Apple TAPI SDK symbol/provider universe
        +
Clang SDK header signatures/type trees
        |
        v
SDK-wide typed compatibility catalog
        |
        v
bulk mechanical ABI generation / coverage

Application validation view
---------------------------
Mach-O import requirements
        |
        v
compare against SDK-wide catalog + semantic-provider status
        |
        v
runtime validation / dynamic discovery
```

The generated execution progression is:

```text
SDK/import type evidence
        |
        v
Clang-proven ARM64 iOS ABI lowering
        |
        v
Clang-proven Win64 carrier lowering
        |
        v
libffi adapter plan
        |
        v
controlled executable cross-ABI bridge
        |
        v
production-independent runtime adapter table
        |
        v
runtime adapter registry / executor
        |
        v
explicit real semantic-provider binding
        |
        v
real loader-selected generated route
```

The important design boundary is that **mechanical ABI translation and semantic compatibility are separate problems**.

The generated side may determine that a function uses `x0`, `v0`, an `x8` indirect result, a Win64 `sret`, a pointer carrier, or aggregate repacking. It does **not** decide that the corresponding iOS API is semantically implemented on Windows. Provider choice, guest pointer validation, callbacks, variadics, stateful Darwin behavior, Objective-C runtime behavior, XPC, graphics, and other subsystem semantics remain explicit runtime work.

The current generator/runtime path therefore follows these rules:

- scan SDK metadata in bulk when producing mechanical coverage; do not wait for every symbol to become a runtime failure
- use the actual Mach-O importer, ordinal, and provider requirement when validating what a particular application resolves
- use Apple TAPI metadata for direct exports and explicit re-export relationships
- use Clang to recover SDK header signatures and compiler-lowered ABI evidence
- preserve Apple LP64 carrier widths instead of recompiling source spellings naively under Win64 LLP64
- let libffi own proven Win64 call mechanics where appropriate
- keep guest stack offsets unknown until they are actually proven
- treat guest pointers as opaque addresses until the runtime validates them
- require explicit semantic ownership before a generated adapter may call a real host implementation
- reject missing, non-executable, or data exports instead of treating export presence as compatibility
- keep callbacks, variadics, no-prototype functions, and unresolved ABI classes outside the generic bridge until their rules are implemented
- keep Objective-C metadata and thread-local/data exports distinct from callable C functions
- never convert unsupported behavior into fake success
- do not use runtime monkey patching as a substitute for a clean compatibility boundary

### What PRs #38–#53 added

- [PR #38](https://github.com/GravAlignLabs/ipasim/pull/38) — machine-readable ARM64 Mach-O import/dependency surface
- [PR #40](https://github.com/GravAlignLabs/ipasim/pull/40) — Apple TAPI SDK provider and re-export knowledge surface
- [PR #41](https://github.com/GravAlignLabs/ipasim/pull/41) — Clang-backed SDK C signature/type surface
- [PR #42](https://github.com/GravAlignLabs/ipasim/pull/42) — deterministic typed compatibility inventory joining importer, provider, and signature evidence
- [PR #43](https://github.com/GravAlignLabs/ipasim/pull/43) — Clang-backed ARM64 iOS/AAPCS64 lowering surface
- [PR #44](https://github.com/GravAlignLabs/ipasim/pull/44) — compiler-backed Win64 carrier lowering and cross-ABI repacking evidence
- [PR #45](https://github.com/GravAlignLabs/ipasim/pull/45) — deterministic libffi-oriented bridge adapter plans
- [PR #46](https://github.com/GravAlignLabs/ipasim/pull/46) — first controlled executable ARM64-to-Win64 libffi bridge proof
- [PR #47](https://github.com/GravAlignLabs/ipasim/pull/47) — documentation of the generated compatibility-engine direction
- [PR #48](https://github.com/GravAlignLabs/ipasim/pull/48) — deterministic production-independent runtime adapter table and generated C++ records
- [PR #49](https://github.com/GravAlignLabs/ipasim/pull/49) — reusable C++ generated-adapter registry/executor with explicit host bindings and pointer validation
- [PR #50](https://github.com/GravAlignLabs/ipasim/pull/50) — first generated adapter bound to an explicitly approved real Darwin semantic provider
- [PR #52](https://github.com/GravAlignLabs/ipasim/pull/52) — first real loader-resolved import routed through its generated adapter and approved semantic provider
- [PR #53](https://github.com/GravAlignLabs/ipasim/pull/53) — table-driven generated semantic import routing with explicit approval data

The current SDK-wide catalog increment adds the missing planning layer above the importer-scoped inventory: all target-matching TAPI symbols can be joined to Clang signature evidence before a specific Mach-O requests them. Typed global C candidates can then be projected into the already validated ABI generator input with **zero runtime requirements**, while ObjC metadata, TLS, mixed metadata, and untyped globals remain explicitly non-callable.

## Compatibility subsystems implemented so far

### ARM64 execution and loader

- real ARM64/AArch64 Unicorn execution on Windows x64
- AAPCS64 register handling and pointer-width host-call translation
- modern ARM64 Mach-O parsing and image loading
- chained fixups and exports-trie resolution
- dependency ordinal handling and RuntimeRoot image resolution
- shared-memory multi-engine execution
- independent secondary guest execution contexts and threaded callbacks
- controlled loader selection of explicitly approved generated semantic routes

### Darwin process, kernel, and memory services

- process identity and selected `proc_pidinfo` behavior backed by Windows process information
- Mach task identity and Mach IPC compatibility work
- monotonic/continuous time and Mach timebase behavior
- `vm_allocate`, `vm_deallocate`, and guest-visible page-size handling
- `__ulock_wait`, `__ulock_wait2`, and `__ulock_wake` synchronization semantics
- libplatform byte/string primitives including bzero, memset/memmove/memcmp families, pattern fills, and adjacent string helpers
- explicit failure for unsupported behavior rather than fabricated success records

### Guest file and descriptor model

- a coherent guest-visible descriptor namespace rather than treating arbitrary Windows descriptors as iOS descriptors
- `open`, `close`, `fcntl`, `lseek`, `read`, `write`, `pread`, and `pwrite`
- `$NOCANCEL` file-syscall aliases where the existing operation already provides the correct non-cancellation boundary
- explicit `ENOTSUP` for `sigsuspend$NOCANCEL` until Darwin signal-mask wait/delivery semantics exist
- `mkfifo` and `mknod` guest namespace support with preserved Darwin type/mode metadata
- FIFO backing through Windows named pipes
- regular-file backing with real Windows handles
- exact ARM64/LP64 Darwin `struct stat` translation for `fstat`, `stat`, and `lstat`
- Windows-backed file identity, link count, size, allocation, block size, and access/write/change/birth timestamps
- guest descriptor lifetime tracking so a closed descriptor is no longer visible and `fstat` correctly returns `EBADF`

### Sockets and host re-exports

- a Darwin socket descriptor registry over WinSock rather than pointer-width `SOCKET` passthrough
- socket creation, connection, and `sendto` translation for the implemented boundary
- target-proven simulator host companion re-exports for `libsystem_sim_kernel`, `libsystem_sim_platform`, and `libsystem_sim_pthread`
- direct ARM64 signatures for the native Darwin host bridge so resolved functions are callable, not merely linkable

### pthread and concurrency work

- pthread QoS encode/decode helpers and direct QoS override behavior
- pthread thread-specific-data key lifecycle and get/set semantics
- pthread workloop create/destroy lifecycle
- pthread workqueue configuration, supported-feature reporting, worker-demand requests, priority forwarding, and override control-plane behavior
- real secondary guest worker execution through a new Windows thread and independent Unicorn guest context
- intentionally conservative feature advertisement: unsupported workqueue delivery modes are not reported as available merely to satisfy a caller

## Selected merged milestones

- [PR #26](https://github.com/GravAlignLabs/ipasim/pull/26) — pthread thread-specific-data semantics
- [PR #28](https://github.com/GravAlignLabs/ipasim/pull/28) — simulator libSystem host re-export support
- [PR #29](https://github.com/GravAlignLabs/ipasim/pull/29) — pthread workloop lifecycle
- [PR #30](https://github.com/GravAlignLabs/ipasim/pull/30) — pthread workqueue subsystem/control plane
- [PR #31](https://github.com/GravAlignLabs/ipasim/pull/31) — shared-memory multi-Unicorn execution proof
- [PR #32](https://github.com/GravAlignLabs/ipasim/pull/32) — independent ARM64 guest worker execution
- [PR #33](https://github.com/GravAlignLabs/ipasim/pull/33) — durable Theos SDK research/provider guidance
- [PR #34](https://github.com/GravAlignLabs/ipasim/pull/34) — reliable self-updating Windows tester packaging
- [PR #35](https://github.com/GravAlignLabs/ipasim/pull/35) — Darwin/libplatform memory and string primitives
- [PR #36](https://github.com/GravAlignLabs/ipasim/pull/36) — Darwin `$NOCANCEL` file syscalls and positional I/O
- [PR #37](https://github.com/GravAlignLabs/ipasim/pull/37) — exact Darwin ARM64 `stat` metadata ABI
- [PR #38](https://github.com/GravAlignLabs/ipasim/pull/38) — ARM64 compatibility surface analyzer
- [PR #40](https://github.com/GravAlignLabs/ipasim/pull/40) — TAPI SDK compatibility knowledge surface
- [PR #41](https://github.com/GravAlignLabs/ipasim/pull/41) — Clang SDK header signature surface
- [PR #42](https://github.com/GravAlignLabs/ipasim/pull/42) — typed compatibility inventory
- [PR #43](https://github.com/GravAlignLabs/ipasim/pull/43) — AAPCS64 ABI lowering surface
- [PR #44](https://github.com/GravAlignLabs/ipasim/pull/44) — Win64 carrier ABI surface
- [PR #45](https://github.com/GravAlignLabs/ipasim/pull/45) — libffi bridge adapter planning surface
- [PR #46](https://github.com/GravAlignLabs/ipasim/pull/46) — controlled executable libffi cross-ABI proof
- [PR #48](https://github.com/GravAlignLabs/ipasim/pull/48) — compile bridge plans into runtime adapter tables
- [PR #49](https://github.com/GravAlignLabs/ipasim/pull/49) — execute generated runtime adapter records
- [PR #50](https://github.com/GravAlignLabs/ipasim/pull/50) — bind generated adapters to real semantic providers
- [PR #52](https://github.com/GravAlignLabs/ipasim/pull/52) — route a real loader-resolved import through the generated semantic-provider path
- [PR #53](https://github.com/GravAlignLabs/ipasim/pull/53) — generalize generated semantic routing into an explicit approval table

These are selected checkpoints rather than a complete changelog. Earlier merged work also established getpid/lseek/read/readlink/sendto boundaries, platform strings, Mach task helpers, pthread CPU/QoS behavior, VM/process support, semantic smokes, synthetic fixtures, loader diagnostics, and the public CI workflow used to validate compatibility increments.

## What remains

The project should still be treated as an active emulator bring-up effort, not a finished modern iOS compatibility layer. Important remaining areas include:

- run the existing AAPCS64 -> Win64 -> libffi generation chain over the SDK-wide typed catalog in deterministic batches and publish mechanical coverage metrics
- build a machine-readable semantic-provider inventory so SDK-wide rows can report approved, candidate, missing, complex, or unsupported semantic status without conflating that status with ABI evidence
- progressively generate production route data from that semantic inventory while preserving explicit approval and fail-closed module/export/address verification
- avoid turning `LiveGuestProfile` or another runtime table into a second handwritten ABI database; generated adapter records should drive general execution as their proven runtime capabilities expand
- exact guest stack placement for ABI cases that overflow ARM64 register banks
- callback/closure trampolines for host-to-guest calls
- variadic and no-prototype runtime boundaries
- broader vector/SIMD and aggregate ABI classes where compiler evidence is not yet sufficient
- additional libSystem/kernel syscall families where real semantics are missing
- broader file, directory, metadata, xattr, process, Mach, and networking behavior
- complete pthread scheduling/event delivery and signal semantics
- Objective-C runtime integration beyond the currently proven bridge paths
- Foundation/UIKit and other framework behavior required by normal applications
- XPC and service-level compatibility
- graphics, UI/event-loop, media, and device-service compatibility

The static/SDK-wide compatibility map is a planning and coverage surface. It is **not** proof that every SDK symbol is callable or semantically implemented.

The semantic/runtime development loop remains:

```text
first genuine non-cascading runtime failure
        -> identify importer/provider/subsystem
        -> implement real semantics
        -> add semantic coverage
        -> rerun
```

The mechanical compatibility-engine loop is deliberately broader:

```text
full target SDK
        -> TAPI provider/export universe
        +  Clang header/type universe
        -> SDK-wide typed catalog
        -> AAPCS64 ABI evidence
        -> Win64 carrier evidence
        -> deterministic libffi bridge plan
        -> generated runtime adapter
        -> compare with explicit semantic-provider inventory
```

A particular Mach-O's imports are then an **overlay on that reusable map**, not the prerequisite for creating it.

## Apple SDK metadata for compatibility research

Use the [Theos SDK archive](https://github.com/theos/sdks) as a public symbol/provider and header reference for SDK-wide compatibility analysis and for investigation of specific runtime boundaries.

Useful iPhoneOS SDK roots currently include:

- [iPhoneOS 9.3](https://github.com/theos/sdks/tree/master/iPhoneOS9.3.sdk)
- [iPhoneOS 10.3](https://github.com/theos/sdks/tree/master/iPhoneOS10.3.sdk)
- [iPhoneOS 11.4](https://github.com/theos/sdks/tree/master/iPhoneOS11.4.sdk)
- [iPhoneOS 12.4](https://github.com/theos/sdks/tree/master/iPhoneOS12.4.sdk)
- [iPhoneOS 13.7](https://github.com/theos/sdks/tree/master/iPhoneOS13.7.sdk)
- [iPhoneOS 14.5](https://github.com/theos/sdks/tree/master/iPhoneOS14.5.sdk)
- [iPhoneOS 15.6](https://github.com/theos/sdks/tree/master/iPhoneOS15.6.sdk)
- [iPhoneOS 16.5](https://github.com/theos/sdks/tree/master/iPhoneOS16.5.sdk)

For libSystem-family research, these are especially useful starting points:

- [iOS 15.6 `libSystem.B.tbd`](https://github.com/theos/sdks/blob/master/iPhoneOS15.6.sdk/usr/lib/libSystem.B.tbd)
- [iOS 16.5 `libSystem.B.tbd`](https://github.com/theos/sdks/blob/master/iPhoneOS16.5.sdk/usr/lib/libSystem.B.tbd)
- [iOS 15.6 `/usr/lib`](https://github.com/theos/sdks/tree/master/iPhoneOS15.6.sdk/usr/lib)
- [iOS 16.5 `/usr/lib`](https://github.com/theos/sdks/tree/master/iPhoneOS16.5.sdk/usr/lib)

`.tbd` files are TAPI text stubs. They are useful for determining:

- exported symbol names
- install names and direct provider libraries
- re-export relationships such as `libSystem.B.dylib` re-exporting subsystem libraries
- target architectures/platforms
- weak, thread-local, and Objective-C metadata classes represented by TAPI
- whether a symbol or subsystem exists across SDK versions

They are **not** implementation source. Do not infer complete behavior, side effects, callback semantics, thread scheduling, ABI details, or a safe no-op implementation from a `.tbd` export alone. Function prototypes require SDK headers/compiler evidence, and correct Windows semantics require independent implementation evidence.

For SDK-wide mechanical analysis, the preferred sequence is:

1. Scan the SDK's `.tbd` files recursively with `tbd_surface.py` to build the target provider/export universe.
2. Scan the SDK headers with `header_surface.py` to build exact Clang-backed C signatures/type trees.
3. Join those surfaces with `sdk_catalog.py` into a deterministic `typed-sdk-catalog`.
4. Keep global typed C functions, untyped globals, Objective-C metadata, TLS, weak exports, mixed metadata, and multi-provider evidence distinct.
5. Project only the mechanically valid typed global C candidates into the existing ABI generator input; do not manufacture runtime requirements or semantic approval.
6. Run the AAPCS64/Win64/libffi stages over that mechanical set in deterministic batches.
7. Compare a particular application's Mach-O imports against the reusable SDK catalog when validating that application.

For semantic/runtime investigation, use a different sequence:

1. Start with the first genuine non-cascading runtime failure.
2. Identify the importing image, dependency ordinal, expected install name, and exact symbol/subsystem.
3. Use the SDK catalog and TAPI re-export graph to understand the surrounding API family.
4. Use Apple open-source implementations, SDK headers, and public behavior evidence to establish semantics.
5. Implement and semantically test the coherent subsystem rather than adding a one-off success shim.
6. Rerun the real loader to verify behavior and expose genuinely dynamic boundaries.

That division keeps SDK-scale automation aggressive on the **mechanical** side while keeping runtime semantics conservative and evidence-driven.

The historical [`mstg/iOS-full-sdk`](https://github.com/mstg/iOS-full-sdk) repository points users to Theos; prefer the maintained Theos SDK collection above for future research.

## Public test strategy

Contributors do **not** need a private commercial application to reproduce compatibility work.

The repository generates its own iOS ARM64 fixtures with Xcode in GitHub Actions:

- **`HelloBootstrap.ipa`** — minimal ARM64 executable used to prove IPA -> Mach-O loader -> SysTranslator -> Unicorn execution on Windows, with expected guest return value `X0=42`
- **`HelloNative.ipa`** — untouched minimal iOS executable used to expose the next genuine Apple runtime boundary
- **`HelloUIKit.ipa`** — small UIKit/Foundation Hello World application used to expose framework/runtime boundaries publicly

Run public validation in this order:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. **Threaded ARM64 Guest Context** — `.github/workflows/threaded-guest-context.yml` when guest-thread/callback work is relevant
4. **Compatibility Surface Analyzer** — `.github/workflows/compat-surface.yml` when changing generated compatibility tooling
5. Optional local testing with another IPA only after the public synthetic/core/tooling tests are understood

If you find a compatibility problem, prefer adding or extending a small synthetic reproduction that can live in this repository. That keeps debugging reproducible for everyone.

## Windows tester update path

The generic Windows tester is published from a green Windows ARM64 Core result and includes `Update-ipaSim-Tester.cmd`.

The updater:

1. runs the replacement operation from a temporary copy so it can update itself safely,
2. downloads the repository-published `BUILD.txt` and tester ZIP from `tester/windows/latest/`,
3. verifies the published SHA256 before installation,
4. extracts into a staging directory,
5. verifies the required tester files, and
6. replaces the installed tester only after verification succeeds.

This gives local compatibility testing a repeatable path to the same green binaries validated by CI rather than relying on manually copied executables from an older build.

## PR-based CI diagnostic loop

The Windows ARM64 Core workflow is designed so a failed build leaves a useful diagnostic directly on the pull request instead of forcing contributors to hunt through a long Actions log.

For the x64 ARM64 emulator-core build, CI currently uses this sequence:

1. Run the verbose CMake/Ninja build with `continue-on-error` long enough to capture its complete output into a temporary log.
2. Preserve the real process exit code; the failure is never converted into success.
3. Extract the first useful compiler/linker/build diagnostics, including `FAILED:`, compiler errors, fatal errors, MSVC `LNK` failures, undefined or unresolved symbols, linker failures, Ninja stops, CMake errors, and named semantic-smoke diagnostics.
4. Keep the first 80 matching diagnostic lines. If no known pattern matches, fall back to the last 120 log lines so an unexpected failure still has context.
5. Write the same diagnostic into the GitHub Actions step summary.
6. On a pull request, publish it to one persistent PR comment containing the hidden marker `<!-- ipasim-core-build-diagnostic -->` and the exact commit SHA that failed.
7. On later pushes, find that marker and **update the existing comment** rather than creating another diagnostic comment. The PR therefore has one current failure report instead of a trail of stale duplicates.
8. After the diagnostic has been published, a separate step exits with failure normally. The GitHub check stays red, so diagnostic reporting cannot hide or suppress a broken build.
9. If a newer commit is pushed while an older run is still executing, Actions cancels the obsolete run through workflow concurrency so CI focuses on the newest checkpoint.

Conceptually, the loop is:

```text
push to PR
   |
   v
build/test
   |
   +---- success --------------------> green check
   |
   +---- failure
           |
           v
      capture full log
           |
           v
      isolate useful diagnostic
           |
           v
      create/update ONE PR comment
      with failing commit SHA
           |
           v
      fail workflow normally
           |
           v
        red check
```

This gives contributors two failure surfaces at the same time: the normal GitHub Actions log for complete detail and a compact, automatically refreshed PR diagnostic for the first actionable error.

The persistent PR-comment collector currently applies specifically to the **x64 ARM64 emulator-core build** in `windows-arm64-core.yml`. Other smoke/test stages still expose failures through their normal Actions logs until the same collector pattern is extended to them. That distinction is intentional: the README should describe what CI actually guarantees today, not what it may support later.

Public diagnostic comments and public bug reports must remain target-neutral. Use the repository-generated synthetic IPAs and public smoke tests for reproduction; do not upload private IPAs, RuntimeRoot contents, private application names, paths, screenshots, or private application logs.

## AI coding agents

[`AGENTS.md`](AGENTS.md) is the canonical instruction set for autonomous and AI-assisted coding work in this repository. Changes under `src/IpaSimulator/` also follow the scoped [`src/IpaSimulator/AGENTS.md`](src/IpaSimulator/AGENTS.md).

Tool-specific entry files exist only to route major coding assistants into those same rules: [`CLAUDE.md`](CLAUDE.md), [`GEMINI.md`](GEMINI.md), and [`.github/copilot-instructions.md`](.github/copilot-instructions.md). Keep policy in `AGENTS.md` rather than duplicating divergent versions for each tool.

Before starting work, inspect `.github/agent-work/` and open/draft PRs. Active claims are coordination metadata on `master`; do not duplicate a subsystem already being worked on.

For mechanical compatibility work, agents should first ask whether the required evidence can be generated from the SDK-wide catalog or an existing generator before creating a per-symbol handwritten mapping. For semantic/runtime work, the first genuine failure remains the truth source.

## Contributing

This repository is public and accepts pull requests from forks. You do not need collaborator access.

A useful contribution can be an SDK-wide compatibility-surface/generator improvement, loader fix, Darwin/Windows semantic bridge, ARM64 ABI correction, synthetic test case, diagnostic improvement, documentation update, or analysis of how another emulator solves a comparable subsystem.

Please keep changes target-neutral and evidence-driven. Do not add application-specific names, paths, patches, or success shims for private binaries.

See [`docs/arm64-ios-compatibility.md`](docs/arm64-ios-compatibility.md) for the current compatibility direction.

---

# Original ipaSim project

This repository contains source code of `ipasim`, an iOS emulator for Windows.
It takes a compiled iOS application and emulates it. However, only the
application's machine code is emulated, whereas system functionality originally
provided by iOS is translated to an equivalent functionality available on
Windows. [More detailed documentation](docs/README.md) is available.

## Historical project status

The original ipaSim implementation supported simple applications. Working samples can be
found in folder [`samples`](samples). For more information about the original
implemented and unimplemented features, see the [author's thesis](docs/thesis/README.md),
its *Conclusion* in particular.

## Related projects

- [touchHLE](https://github.com/touchHLE/touchHLE) — a high-level iOS emulator whose subsystem designs can provide useful comparison points, although its architecture and target era differ from this ARM64 effort

## Cloning the repository

We use [Git Submodules](https://git-scm.com/book/en/v2/Git-Tools-Submodules)
(recursively), so make sure you clone with `--recurse-submodules`. We also use
[Git LFS](https://git-lfs.github.com/), so make sure you have that installed if
you want to get all files. You might also want to use `--depth 1` for a faster
checkout.

## Building and installation

The original project documentation describes the historical build paths:
[build from sources](docs/build.md), [partially prebuilt artifacts](docs/artifacts.md),
and [prebuilt binaries](docs/install.md).

The active ARM64 work is currently validated primarily through the GitHub Actions workflows above. Expect some historical build/install documentation to describe the original architecture until modernization is complete.

## Directory structure

- [`deps`](deps) contains third-party dependencies, mostly as Git submodules.
- [`docs`](docs) contains documentation and research material.
- [`include`](include) has C++ headers of the project.
- [`samples`](samples) contains sources of sample iOS applications and other samples.
- [`scripts`](scripts) contains build and test scripts.
- [`src`](src) contains C++ sources of the project.
  - [HeadersAnalyzer](src/HeadersAnalyzer/README.md) is a compile-time support-code generator from the original architecture.
  - [IpaSimulator](src/IpaSimulator/README.md) is the emulator itself.
  - [objc](src/objc/README.md) contains the project's port of Apple's Objective-C runtime to Windows.

## Original research

[![Poster preview](docs/thesis/poster.png)](docs/thesis/poster.pdf)

- [iOS emulator for Windows](docs/thesis/README.md), a bachelor thesis by [Jan Joneš](https://github.com/jjonescz)
