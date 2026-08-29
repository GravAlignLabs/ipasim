# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). The original project and research remain the foundation of this work. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while preserving explicit diagnostics for behavior that is not yet implemented.

[![Synthetic iOS IPA on Windows](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml)
[![Windows ARM64 Core](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml)

## Current ARM64 work

The modern ARM64 foundation was merged through [PR #3: ARM64 + modern dyld foundation for iOS compatibility](https://github.com/GravAlignLabs/ipasim/pull/3), which preserves 16 substantive engineering milestones across the loader, runtime translation, Darwin compatibility services, testing, and CI. Current work continues from `master`.

Current areas of work include:

- real AArch64 execution through Unicorn on a Windows x64 host
- 64-bit guest register and AAPCS64 handling
- modern ARM64 Mach-O loading
- `LC_DYLD_CHAINED_FIXUPS`, exports trie, dependency ordinals, and RuntimeRoot resolution
- Windows-backed Darwin host compatibility where a defensible semantic mapping exists
- Mach IPC, process, filesystem, socket, VM, and guest descriptor compatibility
- explicit failure for unsupported behavior instead of fake-success stubs or hidden fallbacks
- GitHub Actions diagnostics that expose the first useful failure

### Public test strategy

Contributors do **not** need a private commercial application to reproduce compatibility work.

The repository generates its own iOS ARM64 fixtures with Xcode in GitHub Actions:

- **`HelloBootstrap.ipa`** — minimal ARM64 executable used to prove IPA -> Mach-O loader -> SysTranslator -> Unicorn execution on Windows, with expected guest return value `X0=42`
- **`HelloNative.ipa`** — untouched minimal iOS executable used to expose the next genuine Apple runtime boundary
- **`HelloUIKit.ipa`** — small UIKit/Foundation Hello World application used to expose framework/runtime boundaries publicly

Run public validation in this order:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. Optional local testing with another IPA only after the public synthetic/core tests are understood

If you find a compatibility problem, prefer adding or extending a small synthetic reproduction that can live in this repository. That keeps debugging reproducible for everyone.

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
