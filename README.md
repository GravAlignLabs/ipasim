# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). The original project and research remain the foundation of this work. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while preserving explicit diagnostics for behavior that is not yet implemented.

[![Synthetic iOS IPA on Windows](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml)
[![Windows ARM64 Core](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml)
[![Threaded ARM64 Guest Context](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml)

## Current ARM64 work

The modern ARM64 foundation was merged through [PR #3: ARM64 + modern dyld foundation for iOS compatibility](https://github.com/GravAlignLabs/ipasim/pull/3), which established the first large loader/runtime modernization checkpoint. Development has since advanced through [PR #37: Implement Darwin stat metadata ABI](https://github.com/GravAlignLabs/ipasim/pull/37), with subsystem-level compatibility work, semantic smoke tests, independent guest execution contexts, and a repeatable Windows tester pipeline now established on `master`.

### Verified checkpoint — August 31, 2026

At this checkpoint, the following three public validation paths are green together on the current implementation:

- **Windows ARM64 Core** — builds the x64 Windows emulator core, executes real AArch64 instructions through Unicorn, and runs the Darwin subsystem semantic smoke suite.
- **Threaded ARM64 Guest Context** — executes an ARM64 guest function on an independent Windows host thread with its own Unicorn engine, register state, and guest stack, returning the expected `X0=42`.
- **Synthetic iOS IPA on Windows** — builds public iOS ARM64 fixtures with Xcode and exercises the Windows ipaSim loader/runtime path against them.

The current green state proves the following pieces are real and working together:

- IPA extraction and ARM64 Mach-O loading on Windows
- real AArch64 instruction execution through Unicorn on an x64 Windows host
- 64-bit guest registers and the AAPCS64 integer/pointer call boundary
- modern dyld support including `LC_DYLD_CHAINED_FIXUPS`, exports tries, dependency ordinals, and RuntimeRoot dependency resolution
- shared guest memory mapped into multiple Unicorn engines
- independent guest execution contexts with separate registers and private stacks
- Windows host threads capable of running secondary ARM64 guest callbacks without reusing the main Unicorn CPU state
- thread-local routing of nested guest -> host -> guest callbacks back to the correct guest execution context
- semantic Windows-backed Darwin compatibility services instead of loader-only symbol placeholders
- reproducible public fixtures and a packaged Windows tester that can update itself to the latest published green snapshot with SHA256 verification

This is **not yet a claim that arbitrary modern iOS applications run to completion**. The emulator now advances substantially farther through a modern RuntimeRoot, but additional Darwin, Objective-C, Foundation/UIKit, graphics, event-delivery, and framework behavior remains to be implemented as real runtime boundaries are encountered.

### Compatibility subsystems implemented so far

#### ARM64 execution and loader

- real ARM64/AArch64 Unicorn execution on Windows x64
- AAPCS64 register handling and pointer-width host-call translation
- modern ARM64 Mach-O parsing and image loading
- chained fixups and exports-trie resolution
- dependency ordinal handling and RuntimeRoot image resolution
- shared-memory multi-engine execution
- independent secondary guest execution contexts and threaded callbacks

#### Darwin process, kernel, and memory services

- process identity and selected `proc_pidinfo` behavior backed by Windows process information
- Mach task identity and Mach IPC compatibility work
- monotonic/continuous time and Mach timebase behavior
- `vm_allocate`, `vm_deallocate`, and guest-visible page-size handling
- `__ulock_wait`, `__ulock_wait2`, and `__ulock_wake` synchronization semantics
- libplatform byte/string primitives including bzero, memset/memmove/memcmp families, pattern fills, and adjacent string helpers
- explicit failure for unsupported behavior rather than fabricated success records

#### Guest file and descriptor model

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

#### Sockets and host re-exports

- a Darwin socket descriptor registry over WinSock rather than pointer-width `SOCKET` passthrough
- socket creation, connection, and `sendto` translation for the implemented boundary
- target-proven simulator host companion re-exports for `libsystem_sim_kernel`, `libsystem_sim_platform`, and `libsystem_sim_pthread`
- direct ARM64 signatures for the native Darwin host bridge so resolved functions are callable, not merely linkable

#### pthread and concurrency work

- pthread QoS encode/decode helpers and direct QoS override behavior
- pthread thread-specific-data key lifecycle and get/set semantics
- pthread workloop create/destroy lifecycle
- pthread workqueue configuration, supported-feature reporting, worker-demand requests, priority forwarding, and override control-plane behavior
- real secondary guest worker execution through a new Windows thread and independent Unicorn guest context
- intentionally conservative feature advertisement: unsupported workqueue delivery modes are not reported as available merely to satisfy a caller

### Selected merged milestones after the initial ARM64 foundation

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

These are selected checkpoints rather than a complete changelog. Earlier merged work also established getpid/lseek/read/readlink/sendto boundaries, platform strings, Mach task helpers, pthread CPU/QoS behavior, VM/process support, semantic smokes, synthetic fixtures, loader diagnostics, and the public CI workflow used to identify each next compatibility boundary.

### What remains

The project should still be treated as an active emulator bring-up effort, not a finished modern iOS compatibility layer. Important remaining areas include:

- additional libSystem/kernel syscall families as the real loader reaches them
- broader file, directory, metadata, xattr, process, Mach, and networking behavior
- complete pthread scheduling/event delivery and signal semantics
- Objective-C runtime integration beyond the currently proven bridge paths
- Foundation/UIKit and other framework behavior required by normal applications
- graphics, UI/event-loop, media, and device-service compatibility
- additional ARM64 ABI classes such as FP/SIMD and aggregate calling conventions where target code requires them

The static symbol audit intentionally reports a much broader missing compatibility surface than the current runtime may immediately require. Treat it as an independent inventory and prioritization aid, **not** as proof that every reported symbol is the next runtime blocker. The development loop remains: fix the first genuine non-cascading loader/runtime boundary, implement the coherent subsystem behind it, add semantic coverage, and rerun.

Current areas of work continue to emphasize:

- real AArch64 execution through Unicorn on a Windows x64 host
- modern ARM64 Mach-O and dyld compatibility
- Windows-backed Darwin behavior where a defensible semantic mapping exists
- subsystem-level implementations instead of one-symbol fake-success stubs
- public synthetic reproductions and semantic tests
- GitHub Actions diagnostics that expose the first useful failure

### Apple SDK metadata for compatibility research

When a modern Darwin/iOS runtime boundary is encountered, use the [Theos SDK archive](https://github.com/theos/sdks) as a symbol/provider reference before treating the failure as an isolated one-off symbol.

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
- install names and likely provider libraries
- re-export relationships such as `libSystem.B.dylib` re-exporting subsystem libraries
- target architectures/platforms
- whether a symbol or subsystem exists across SDK versions

They are **not** implementation source. Do not infer complete behavior, side effects, callback semantics, thread scheduling, ABI details, or a safe no-op implementation from a `.tbd` export alone. Function prototypes may also require SDK headers or source-level confirmation.

For AI-assisted compatibility work, use this sequence:

1. Start with the first genuine loader/runtime failure from the current tester rather than the static audit ranking alone.
2. Identify the importing image, dependency ordinal, expected install name, and exact symbol family.
3. Cross-reference the corresponding Theos `.tbd` files across nearby SDK versions to identify the provider, re-exports, and adjacent subsystem symbols.
4. Inspect the actual RuntimeRoot image imports so the implementation batch reflects what the target runtime really requests.
5. Treat related symbols as a subsystem when evidence supports it instead of repeatedly implementing one symbol per test run.
6. Use Apple open-source implementation repositories and SDK headers for behavior, constants, prototypes, and ABI semantics before writing the Windows bridge.
7. Add semantic tests for the implemented subsystem and continue to fail explicitly for unsupported behavior.
8. Keep the static symbol audit as an independent observer. Do not modify audit interpretation or suppress audit output merely to make a runtime compatibility change appear complete.

A practical research flow is therefore:

```text
runtime failure
    -> exact importer / ordinal / symbol
    -> Theos SDK provider + version cross-reference
    -> enumerate the coherent subsystem boundary
    -> Apple source + headers for semantics
    -> implement and semantically test the subsystem
    -> run the real loader again
```

The historical [`mstg/iOS-full-sdk`](https://github.com/mstg/iOS-full-sdk) repository points users to Theos; prefer the maintained Theos SDK collection above for future research.

### Public test strategy

Contributors do **not** need a private commercial application to reproduce compatibility work.

The repository generates its own iOS ARM64 fixtures with Xcode in GitHub Actions:

- **`HelloBootstrap.ipa`** — minimal ARM64 executable used to prove IPA -> Mach-O loader -> SysTranslator -> Unicorn execution on Windows, with expected guest return value `X0=42`
- **`HelloNative.ipa`** — untouched minimal iOS executable used to expose the next genuine Apple runtime boundary
- **`HelloUIKit.ipa`** — small UIKit/Foundation Hello World application used to expose framework/runtime boundaries publicly

Run public validation in this order:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. **Threaded ARM64 Guest Context** — `.github/workflows/threaded-guest-context.yml`
4. Optional local testing with another IPA only after the public synthetic/core tests are understood

If you find a compatibility problem, prefer adding or extending a small synthetic reproduction that can live in this repository. That keeps debugging reproducible for everyone.

### Windows tester update path

The generic Windows tester is published from a green Windows ARM64 Core result and includes `Update-ipaSim-Tester.cmd`.

The updater:

1. runs the replacement operation from a temporary copy so it can update itself safely,
2. downloads the repository-published `BUILD.txt` and tester ZIP from `tester/windows/latest/`,
3. verifies the published SHA256 before installation,
4. extracts into a staging directory,
5. verifies the required tester files, and
6. replaces the installed tester only after verification succeeds.

This gives local compatibility testing a repeatable path to the same green binaries validated by CI rather than relying on manually copied executables from an older build.

### PR-based CI diagnostic loop

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

### AI coding agents

[`AGENTS.md`](AGENTS.md) is the canonical instruction set for autonomous and AI-assisted coding work in this repository. Changes under `src/IpaSimulator/` also follow the scoped [`src/IpaSimulator/AGENTS.md`](src/IpaSimulator/AGENTS.md).

Tool-specific entry files exist only to route major coding assistants into those same rules: [`CLAUDE.md`](CLAUDE.md), [`GEMINI.md`](GEMINI.md), and [`.github/copilot-instructions.md`](.github/copilot-instructions.md). Keep policy in `AGENTS.md` rather than duplicating divergent versions for each tool.

### Contributing

This repository is public and accepts pull requests from forks. You do not need collaborator access.

A useful contribution can be a loader fix, Darwin/Windows semantic bridge, ARM64 ABI correction, synthetic test case, diagnostic improvement, documentation update, or analysis of how another emulator solves a comparable subsystem.

Please keep changes target-neutral and evidence-driven. Do not add application-specific names, paths, patches, or success shims for private binaries.

See [`docs/arm64-ios-compatibility.md`](docs/arm64-ios-compatibility.md) for the current compatibility direction.

---

## Original ipaSim project

This repository contains source code of `ipasim`, an iOS emulator for Windows.
It takes a compiled iOS application and emulates it. However, only the
application's machine code is emulated, whereas system functionality originally
provided by iOS is translated to an equivalent functionality available on
Windows. [More detailed documentation](docs/README.md) is available.

### Historical project status

The original ipaSim implementation supported simple applications. Working samples can be
found in folder [`samples`](samples). For more information about the original
implemented and unimplemented features, see the [author's thesis](docs/thesis/README.md),
its *Conclusion* in particular.

### Related projects

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
