# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). The original project and research remain the foundation of this work. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while preserving explicit diagnostics for behavior that is not yet implemented.

[![Synthetic iOS IPA on Windows](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml/badge.svg?branch=feature%2Farm64-ios-compatibility)](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml)
[![Windows ARM64 Core](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml/badge.svg?branch=feature%2Farm64-ios-compatibility)](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml)

## Current ARM64 work

The active development work is tracked in [ARM64 + modern dyld foundation for iOS compatibility](https://github.com/GravAlignLabs/ipasim/pull/3).

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
