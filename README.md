# ipaSim

> **Active fork: modern ARM64 iOS compatibility on Windows**
>
> This repository is a fork of [`ipasimulator/ipasim`](https://github.com/ipasimulator/ipasim). The original project and Jan Joneš's research remain the foundation of this work. This fork is extending ipaSim toward modern 64-bit ARM64 iOS applications while preserving explicit diagnostics for behavior that is not yet implemented.

[![Synthetic iOS IPA on Windows](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/synthetic-hello-ipa.yml)
[![Windows ARM64 Core](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/windows-arm64-core.yml)
[![Threaded ARM64 Guest Context](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml/badge.svg?branch=master)](https://github.com/GravAlignLabs/ipasim/actions/workflows/threaded-guest-context.yml)

## Project north star

ipaSim is **not** being developed as a sequence of application-specific symbol fixes.

The long-term objective is to derive the **mechanical iOS compatibility surface from Apple SDK metadata and compiler evidence in bulk**, generate reusable ARM64-to-Win64 adapter records, and maintain a separate, explicitly validated semantic-provider catalog. A real IPA is primarily a validation target and a source of dynamic/behavioral evidence; it should not be the primary mechanism for discovering which mechanically describable SDK APIs exist.

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

**Latest merged compatibility-engine checkpoint: [PR #61 — Migrate process identity calls to generated routing](https://github.com/GravAlignLabs/ipasim/pull/61).**

PR #58 proved the complete pinned iPhoneOS16.5 SDK mechanical preflight. PR #59 turned existing host exports plus generated adapters into a deterministic semantic-migration planning surface without granting approval automatically. PR #60 removed the `_getpid`-specific execution profile by making `SysTranslator` capture and commit the live ARM64 state required by each generated `AdapterRecord`. PR #61 then proved the first multi-symbol production migration: `_getpid`, `_getuid`, `_geteuid`, `_getgid`, and `_getegid` now execute through the same generated route and the four credential calls were removed from `SysTranslator`'s handwritten Darwin ABI table.

The generated production path is now:

```text
real Mach-O import resolution
        -> normal PE export resolution
        -> explicit semantic-approval table lookup
        -> exact provider module/export/address verification
        -> generated AdapterRecord requirements
        -> live AAPCS64 GPR/SIMD/stack capture as required
        -> generated libffi execution
        -> generated result/state commit
        -> ARM64 guest execution resumes
```

There is also parallel Darwin pthread work. Before changing pthread, `SysTranslator.cpp`, `IpaSimulator.cpp`, `src/IpaSimulator/CMakeLists.txt`, or nearby guest-thread lifecycle code, inspect `.github/agent-work/` and open PRs to avoid overlapping active work.

## Current ARM64 work

The modern ARM64 foundation began with [PR #3](https://github.com/GravAlignLabs/ipasim/pull/3). The complete SDK-wide mechanical compatibility preflight is merged, the generic live generated-adapter executor is in production, and the first five process-identity imports have been migrated away from handwritten ABI routing.

The project is working on two connected layers:

1. real Windows-backed Darwin/runtime implementation required by modern iOS binaries; and
2. an SDK/compiler-driven compatibility engine that generates the mechanical ABI bridge in bulk instead of hand-writing one symbol signature at a time.

### Successful full-SDK preflight checkpoint — September 1, 2026

PR #58 exercises the full compatibility pipeline against `theos/sdks@0222fd5413cf4b9af096f37b4621afa2688572f7`, scoped to `iPhoneOS16.5.sdk`.

The authoritative physical header inventory is **5,118 headers**. CI partitions it into **32 deterministic shards** and accepts a merged header surface only when every physical header is owned exactly once. There are no bad-header ignore lists and no partial-success semantics.

At commit `fbe336d0b4060766dc498f8a7757097c07c79fc3`, **Compatibility Surface Analyzer run #103 passed** and the authoritative **Theos iPhoneOS16.5 SDK Preflight run #55 passed end to end**. Subsequent semantic-scaling changes through PR #61 continue to pass the authoritative full-SDK preflight.

The successful run produced the following mechanical coverage:

- exhaustive physical headers analyzed: **5,118**
- TAPI symbols: **1,355,229**
- Clang header C signatures: **13,795**
- SDK catalog symbols: **1,354,457**
- typed C candidates: **13,298**
- AAPCS64 generated candidates: **12,599**
- Win64 cross-ABI candidates: **12,515**
- generated runtime adapters: **10,599**
- explicitly approved generated production routes: **5**
- catalog rows not yet semantically approved: **1,354,452**

The large difference between the SDK catalog size and the typed C candidate count is intentional. The catalog keeps callable C exports distinct from Objective-C metadata, TLS/data records, weak/mixed metadata, untyped globals, and other non-callable evidence instead of pretending every exported SDK record can become a function adapter.

Current pipeline state:

```text
pinned iPhoneOS16.5.sdk
        -> complete TAPI scan                    PASS: 1,355,229 symbols
        -> exhaustive Clang header indexing      PASS: 5,118/5,118 headers
        -> merged header C signatures            PASS: 13,795
        -> SDK typed catalog                     PASS: 1,354,457 records
        -> typed C projection                    PASS: 13,298 candidates
        -> AAPCS64 lowering                      PASS: 12,599 generated candidates
        -> Win64 carrier lowering                PASS: 12,515 cross-ABI candidates
        -> libffi bridge plans                   PASS
        -> generated runtime adapters            PASS: 10,599 adapters
        -> compatibility planner                 PASS
        -> semantic-route comparison             PASS / fail-closed approval boundary
        -> generic live generated execution      PASS
        -> approved production routes            PASS: 5 process-identity imports
```

The successful workflow published the complete generated compatibility bundle as the `theos-iphoneos16.5-sdk-compatibility` artifact in run #55. The bundle contains the TAPI surface, header signatures, SDK catalog, AAPCS64 and Win64 manifests, bridge plan, runtime adapter table, compatibility plan, generated adapter include, and approved semantic route include.

Passing this preflight does **not** mean 10,599 iOS APIs are semantically implemented on Windows. It means the mechanical SDK/compiler pipeline can describe and carry that proven subset through the complete generation path without weakening validation. Semantic-provider approval remains separate and conservative.

The preflight remains intentionally fail-closed. Compiler/header/context failures, ABI invariant failures, unsupported carrier shapes, and missing semantic approval remain visible evidence rather than being converted into success.

### SDK context recovery learned during PR #58

The exhaustive run exposed and fixed several generic SDK/compiler-context classes rather than adding header-specific exceptions:

- framework leaves are entered through SDK-derived umbrella/public ownership while declarations remain attributed to the physical target header;
- nested framework include ownership can be followed transitively through SDK-authored include/import edges;
- guarded declaration leaves may be re-entered safely when an umbrella edge is target-conditional, while unguarded implementation leaves are not entered twice;
- libc++ is entered through the SDK's own `usr/include/c++/v1` root;
- explicit SDK-authored public-header recommendations and reverse include owners are used as evidence;
- module guards are distinguished from genuine module imports;
- actual `@import` failures are checked against the SDK module maps;
- Swift shim headers can use their SDK-authored importer branch when appropriate;
- package-style `usr/include/<name>/<name>.h` ownership and prerequisite typedef providers can be recovered from SDK/compiler evidence;
- target-inactive headers remain narrowly evidence-based rather than being converted into success;
- AAPCS64 probes preserve required SDK umbrella context;
- SDK convenience macros that alias an exported C function to an inline helper are removed only after the owning declaration has been entered, so ABI probing still targets the real exported function;
- AAPCS64 batching records all batch failures before failing closed, improving one-run diagnostics without approving partial output; and
- Win64 batch canonicalization now preserves the complete compiler-generated carrier namespace, including intermediate carrier typedef numbers that may disappear from the public LLVM-derived manifest.

This work remains mechanical SDK analysis only. It does not grant semantic-provider approval, alter loader policy, or claim that an exported SDK function is correctly implemented on Windows merely because its ABI is mechanically describable.

## Historical Apple toolchain evidence

The public [`lujingyu/UWin`](https://github.com/lujingyu/UWin) repository provides an unusually useful historical reference corpus for understanding long-lived Apple SDK conventions. It is effectively a frozen 2011 iOS application snapshot with source, Xcode project state, committed build metadata, old CodeSense indexes, and some third-party build artifacts.

The useful evidence is historical and external; **none of it is copied into ipaSim as implementation or semantic approval**.

### What the UWin snapshot preserves

- Xcode CodeSense/index state referencing the iPhone Simulator 4.2 SDK.
- Real device build state referencing `iPhoneOS4.2.sdk` and Apple's GCC 4.2 toolchain.
- Successful device compilation for both **ARMv6 and ARMv7**.
- Translation-unit include evidence from an actual period iOS build.
- Real BSD/Darwin networking source using APIs and layouts such as `socket`, `ioctl`, `SIOCGIFCONF`, `SIOCGIFFLAGS`, `struct ifreq`, `sockaddr_in`, `AF_LINK`, `sockaddr_dl`, `LLADDR`, `ether_ntoa`, `sa_len`, and related interfaces.
- An external fat static archive, `libOAuth.a`, with ARMv6, ARMv7, and i386 slices. It is useful as a historical format/architecture specimen, but it should **not** be redistributed into ipaSim because the UWin repository does not provide a clear repository-level redistribution license for that artifact.

One especially useful finding is the historical include order in UWin's `Reachability.m`:

```text
sys/socket.h
    -> netinet/in.h
    -> netinet6/in6.h
    -> arpa/inet.h
```

The committed iOS 4.2 build state independently records `netinet/in.h` in that successfully compiled translation unit. That gives us evidence that the `netinet/in.h` -> `netinet6/in6.h` relationship encountered by the modern iOS 16.5 scanner is a long-standing Darwin/Apple SDK convention rather than a one-off Theos packaging quirk.

### How this research should be used

Historical compiler-success evidence can help distinguish enduring Apple SDK structure from a current SDK-specific accident:

```text
historical Apple/Xcode compiler evidence
             |
             +-> include ownership/order
             +-> SDK macro/context conventions
             +-> real ARM build targets
             +-> actual Darwin API usage
             |
             v
compare structurally with modern SDK evidence
             |
             v
make the generic compatibility analyzer more accurate
```

The UWin `.pbxindex` also contains `cdecls`, `decls`, `imports`, `refs`, `files`, `categories`, `protocols`, `subclasses`, and symbol databases. That old CodeSense format is undocumented, so the more immediately useful artifact is its readable build-state evidence, which records compiler/SDK/architecture/translation-unit relationships directly.

The snapshot also carries old `.svn` metadata for third-party libraries copied from another working tree. Treat those files as historical build examples, **not** authoritative upstream provenance.

A future research tool may extract historical include/import relationships into a small reference corpus. Such a corpus should contain derived structural evidence only; it should not vendor UWin source or binary artifacts.

## Current generated cross-ABI compatibility engine

There are two useful views of the same compatibility knowledge:

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
        -> Clang-proven ARM64 iOS ABI lowering
        -> Clang-proven Win64 carrier lowering
        -> libffi adapter plan
        -> controlled executable cross-ABI bridge
        -> production-independent runtime adapter table
        -> runtime adapter registry / executor
        -> explicit real semantic-provider binding
        -> real loader-selected generated route
        -> live generated ARM64 state capture/commit
```

The generated side may determine that a function uses `x0`, `v0`, an `x8` indirect result, a Win64 `sret`, a pointer carrier, or aggregate repacking. It does **not** decide that the corresponding iOS API is semantically implemented on Windows.

Core rules:

- scan SDK metadata in bulk when producing mechanical coverage;
- use Apple TAPI metadata for direct exports and explicit re-export relationships;
- use Clang to recover SDK header signatures and compiler-lowered ABI evidence;
- preserve Apple LP64 carrier widths instead of naively recompiling source spellings under Win64 LLP64;
- let libffi own proven Win64 call mechanics where appropriate;
- keep guest stack offsets unknown until they are proven;
- treat guest pointers as opaque addresses until runtime validation;
- require explicit semantic ownership before a generated adapter may call a real host implementation;
- reject missing, non-executable, or data exports instead of treating export presence as compatibility;
- keep callbacks, variadics, no-prototype functions, and unresolved ABI classes outside the generic bridge until their rules are implemented;
- keep Objective-C metadata and thread-local/data exports distinct from callable C functions; and
- never convert unsupported behavior into fake success.

### Compatibility-engine milestones

- [PR #38](https://github.com/GravAlignLabs/ipasim/pull/38) — machine-readable ARM64 Mach-O import/dependency surface
- [PR #40](https://github.com/GravAlignLabs/ipasim/pull/40) — Apple TAPI SDK provider and re-export knowledge surface
- [PR #41](https://github.com/GravAlignLabs/ipasim/pull/41) — Clang-backed SDK C signature/type surface
- [PR #42](https://github.com/GravAlignLabs/ipasim/pull/42) — deterministic typed compatibility inventory
- [PR #43](https://github.com/GravAlignLabs/ipasim/pull/43) — Clang-backed ARM64 iOS/AAPCS64 lowering
- [PR #44](https://github.com/GravAlignLabs/ipasim/pull/44) — compiler-backed Win64 carrier lowering
- [PR #45](https://github.com/GravAlignLabs/ipasim/pull/45) — deterministic libffi-oriented bridge plans
- [PR #46](https://github.com/GravAlignLabs/ipasim/pull/46) — first controlled executable ARM64-to-Win64 libffi proof
- [PR #48](https://github.com/GravAlignLabs/ipasim/pull/48) — production-independent runtime adapter table
- [PR #49](https://github.com/GravAlignLabs/ipasim/pull/49) — generated-adapter registry/executor
- [PR #50](https://github.com/GravAlignLabs/ipasim/pull/50) — generated adapter bound to an explicitly approved real Darwin semantic provider
- [PR #52](https://github.com/GravAlignLabs/ipasim/pull/52) — real loader-resolved import routed through generated ABI + approved semantic provider
- [PR #53](https://github.com/GravAlignLabs/ipasim/pull/53) — table-driven generated semantic import routing
- [PR #58](https://github.com/GravAlignLabs/ipasim/pull/58) — pinned full-SDK compatibility preflight, generic SDK-context hardening, and successful end-to-end iPhoneOS16.5 mechanical coverage
- [PR #59](https://github.com/GravAlignLabs/ipasim/pull/59) — deterministic semantic migration candidates derived from real host exports plus generated adapters without granting approval
- [PR #60](https://github.com/GravAlignLabs/ipasim/pull/60) — generic live ARM64 generated-adapter execution in `SysTranslator`
- [PR #61](https://github.com/GravAlignLabs/ipasim/pull/61) — first multi-symbol production migration: process identity calls moved off the handwritten Darwin ABI table

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
- generated live GPR/SIMD/stack capture and result commit driven by `AdapterRecord`

### Darwin/runtime work

Implemented coverage includes process identity and selected process information, Mach task/time/VM behavior, ulock synchronization, libplatform memory/string primitives, a guest-visible file-descriptor namespace, regular files/FIFOs, Darwin ARM64 `stat` translation, socket/WinSock translation for the implemented boundary, selected simulator host re-exports, pthread QoS/TSD/workloop/workqueue work, and independent guest worker execution.

Unsupported behavior remains explicit rather than being fabricated as success.

## Generated semantic-routing completion roadmap

The current scaling phase has a concrete completion criterion: **every callable `IpaSimDarwinHost.dll` boundary that the guest can reach should either use a generated SDK-backed adapter plus explicit semantic approval, or be explicitly classified as complex/unsupported. The handwritten `DarwinHostCallSignature` table should then disappear rather than surviving as a second ABI database.**

Planned PR numbers below describe the intended order; the exact number can shift if an unrelated PR lands first. Each PR should remain short-lived, preserve full public CI, and move a coherent semantic family rather than one symbol at a time.

### Planned PR #62 — scalar descriptor calls

Migrate the simplest argument-bearing descriptor operations first, starting with `close` and `lseek` where the existing Windows-backed semantics and public smokes are already strong. This PR should prove that generated production routing handles nonzero GPR arguments and a 64-bit scalar result without any handwritten `ArgCount`/`Returns` record. Keep pointer-bearing and variadic calls out of this PR.

**Exit condition:** `close` and `lseek` are explicitly approved, executed through generated adapters, covered by production smoke tests, and removed from the handwritten table.

### Planned PR #63 — buffer and positional I/O

Migrate `read`, `write`, `pread`, and `pwrite`, including the applicable `$NOCANCEL` aliases only when their exact SDK/export identities are mechanically present. This is the first production batch that should exercise generated pointer-bearing arguments. Pointer validation, byte-count behavior, 64-bit offsets, EOF/error behavior, and unchanged file-position semantics for positional I/O must remain explicit.

**Exit condition:** the normal buffer-I/O path is generated and semantically approved with no per-symbol ABI restatement in `SysTranslator`.

### Planned PR #64 — filesystem metadata and paths

Migrate the implemented fixed-layout filesystem/path family such as `fstat`, `stat`, `lstat`, `readlink`, `mkfifo`, and `mknod` where the SDK-generated adapters are ready and the existing Darwin translations are defensible. Preserve Darwin structure layout and path/error semantics; do not let host structure layout leak into guest memory.

**Exit condition:** implemented filesystem metadata/path exports no longer depend on handwritten host-call signatures.

### Planned PR #65 — process, time, and simple Mach boundaries

Migrate already implemented fixed-signature process/time/Mach calls such as `proc_pidpath`, `pid_for_task`, `mach_continuous_time`, and `mach_timebase_info` when the generated SDK records and exact host providers agree. Data exports such as `mach_task_self_` and `vm_page_size` remain data and must never enter callable routing.

**Exit condition:** the straightforward process/time boundaries are generated while callable-vs-data classification remains fail-closed.

### Planned PR #66 — Mach IPC and guest-stack proof

Use `mach_msg_overwrite` or the next equivalent proven Mach boundary to exercise a production generated adapter whose AAPCS64 arguments extend beyond `X0`-`X7`. This PR should prove exact guest-stack capture/commit from generated requirements rather than special-casing the ninth argument in `SysTranslator`.

**Exit condition:** the old handwritten nine-argument Mach routing can be removed and stack-spilled arguments are proven by executable regression tests.

### Planned PR #67 — libplatform bulk primitives

Migrate the implemented `_platform_*` memory/string family as a high-volume coherent batch. These are valuable because many already have direct host semantics but currently consume handwritten call signatures. Validate pointer direction, lengths, void/scalar returns, and aliases through the generated records rather than by symbol-name assumptions.

**Exit condition:** the implemented libplatform primitive family is generated, approved, and absent from the handwritten ABI table.

### Planned PR #68 — sockets and network calls

Migrate `socket`, `connect`, `__sendto`, and other already implemented fixed-signature network boundaries whose Darwin-to-Winsock translations are covered by smoke tests. Preserve Darwin sockaddr/flag translation and guest descriptor ownership instead of treating Winsock descriptors as interchangeable native integers.

**Exit condition:** implemented fixed-signature socket calls use generated ABI execution while semantic translation remains owned by the socket subsystem.

### Planned PR #69 — synchronization and pthread families

After any overlapping pthread/guest-thread lifecycle work lands, migrate mechanically ready ulock, pthread QoS, TSD, workloop, and workqueue entry points in semantic groups. Do not mass-approve workgroup or callback-taking functions merely because an adapter exists; functions requiring guest callbacks remain blocked on the callback architecture.

**Exit condition:** fixed-signature implemented synchronization/pthread providers use generated routing, while callback-dependent or unsupported behavior remains explicitly classified.

### Planned PR #70 — variadic and no-prototype runtime support

Implement a compiler-evidence-driven runtime policy for variadic/no-prototype boundaries before migrating calls such as `open` or `fcntl`. The generated bridge must know which runtime arguments actually exist; hard-coding an always-three-argument interpretation would simply recreate the old table in another form.

**Exit condition:** supported variadic calls have an explicit, tested ABI mechanism and unsupported forms still fail closed.

### Planned PR #71 — general host-to-guest callbacks

Generalize callback/closure translation so native semantic providers can safely invoke ARM64 guest function pointers using compiler/generated ABI evidence. This is required for broader pthread/workgroup/runtime APIs and must support lifetime, thread/context ownership, GPR/SIMD/stack state, and return propagation without one-off callback signatures.

**Exit condition:** callback-taking semantic providers can use one general mechanism, and callback ABI metadata is not duplicated manually.

### Planned PR #72 — remove the handwritten Darwin ABI table

Audit the remaining `IpaSimDarwinHost.dll` callable exports against the semantic migration plan. Every reachable callable export must be either an explicitly approved generated route or an explicitly classified complex/unsupported boundary. Add a CI invariant that prevents a new handwritten Darwin host ABI entry from silently reappearing, then delete `DarwinHostCallSignature` and its lookup path from `SysTranslator`.

**Exit condition:** `SysTranslator` contains no second handwritten Darwin ABI database. Generated adapter records are the mechanical source of truth; semantic approval is the policy source of truth; unsupported/complex boundaries remain visible.

### After the scaling phase

Deleting the handwritten table does not make ipaSim a finished iOS runtime. It marks the end of the **mechanical host-call scaling problem**. Development then follows the first genuine runtime boundary exposed by public fixtures and real IPAs, with larger semantic work continuing in Objective-C/Swift runtime behavior, Foundation/UIKit, XPC/services, graphics/UI/event loops, media, networking depth, and device services.

## What remains

ipaSim is still an active emulator bring-up effort, not a finished modern iOS compatibility layer. The complete pinned SDK mechanical preflight is proven and generated live execution is now scaling across real providers; the major remaining work is turning mechanically describable coverage into correct runtime behavior without weakening the semantic-approval boundary.

Important remaining areas include:

- execute the generated semantic-routing roadmap above until the handwritten Darwin ABI table is eliminated;
- expand the machine-readable semantic-provider inventory so SDK-wide rows can move deliberately from `unclassified` / `not-approved` into approved, candidate, complex, or unsupported semantic states;
- progressively generate more production route data from that semantic inventory while preserving explicit module/export/address verification;
- keep generated adapter records as the mechanical source of truth instead of creating another handwritten ABI database;
- exact guest-stack placement for remaining ABI classes that overflow ARM64 register banks;
- callback/closure trampolines for host-to-guest calls;
- variadic and no-prototype runtime boundaries;
- broader vector/SIMD and aggregate ABI classes where compiler evidence is not yet sufficient;
- broader libSystem, file, directory, xattr, process, Mach, networking, pthread, and signal semantics;
- Objective-C and Swift runtime integration beyond the currently proven paths;
- Foundation/UIKit and other framework behavior;
- XPC and service-level compatibility; and
- graphics, UI/event-loop, media, and device-service compatibility.

The static/SDK-wide compatibility map is a planning and coverage surface. It is **not** proof that every SDK symbol is callable or semantically implemented.

## Apple SDK metadata for compatibility research

Use the maintained [Theos SDK archive](https://github.com/theos/sdks) as the public symbol/provider/header reference for SDK-wide analysis. The authoritative preflight pins a specific commit and `iPhoneOS16.5.sdk` so results cannot silently drift with upstream changes.

`.tbd` files are useful for exported names, install names, provider/re-export relationships, targets, weak/TLS/Objective-C metadata classes, and SDK-version comparisons. They are **not implementation source**. Function prototypes require SDK headers/compiler evidence, and correct Windows semantics require independent implementation evidence.

Preferred SDK-wide mechanical sequence:

1. scan `.tbd` files recursively with `tbd_surface.py`;
2. scan every physical SDK header through the exhaustive/contextual header pipeline;
3. join provider/export and header/type evidence with `sdk_catalog.py`;
4. keep callable C exports, untyped globals, Objective-C metadata, TLS, weak exports, and mixed metadata distinct;
5. project only mechanically valid typed C exports into ABI generation;
6. lower deterministically through AAPCS64, Win64, and libffi planning; and
7. compare application imports and explicit semantic-provider status against that reusable map.

## Public test strategy

Contributors do **not** need a private commercial application to reproduce compatibility work.

The repository generates public ARM64 fixtures in GitHub Actions:

- **`HelloBootstrap.ipa`** — minimal loader/Unicorn proof with expected guest return `X0=42`
- **`HelloNative.ipa`** — untouched minimal iOS executable used to expose the next Apple runtime boundary
- **`HelloUIKit.ipa`** — small UIKit/Foundation application used to expose framework/runtime boundaries publicly

Run public validation in this order when relevant:

1. **Synthetic iOS IPA on Windows** — `.github/workflows/synthetic-hello-ipa.yml`
2. **Windows ARM64 Core** — `.github/workflows/windows-arm64-core.yml`
3. **Threaded ARM64 Guest Context** — `.github/workflows/threaded-guest-context.yml`
4. **Compatibility Surface Analyzer** — `.github/workflows/compat-surface.yml`
5. **Theos iPhoneOS16.5 SDK Preflight** — `.github/workflows/theos-sdk-preflight.yml` for SDK-wide compatibility-engine changes

If you find a compatibility problem, prefer a small public synthetic or compiler-backed regression over application-specific handling.

## Local full-SDK preflight

PR #58 also carries a resumable local pipeline for Windows/WSL development. The root helpers prepare the pinned SDK, run the compatibility-surface tests, reuse validated shard evidence where appropriate, and continue the downstream SDK pipeline without weakening the CI invariants.

See [`LOCAL_THEOS_PREFLIGHT.md`](LOCAL_THEOS_PREFLIGHT.md) for the current commands and prerequisites.

## AI coding agents

[`AGENTS.md`](AGENTS.md) is the canonical instruction set for autonomous and AI-assisted coding work. Changes under `src/IpaSimulator/` also follow [`src/IpaSimulator/AGENTS.md`](src/IpaSimulator/AGENTS.md).

Before starting work, inspect `.github/agent-work/` and open/draft PRs. For mechanical compatibility work, first ask whether the required evidence can be generated from the SDK-wide catalog or an existing generator before creating a per-symbol handwritten mapping. For semantic/runtime work, the first genuine non-cascading failure remains the truth source.

Please keep changes target-neutral and evidence-driven. Do not add private application names, paths, patches, or success shims.

See [`docs/arm64-ios-compatibility.md`](docs/arm64-ios-compatibility.md) for the current compatibility direction.

---

# Original ipaSim project

This repository contains source code of `ipasim`, an iOS emulator for Windows. It takes a compiled iOS application and emulates the application's machine code while translating system functionality to equivalent functionality available on Windows. [More detailed historical documentation](docs/README.md) is available.

The original implementation supported simple applications. See the [author's thesis](docs/thesis/README.md), especially its conclusion, for the original project's implemented and unimplemented scope.

## Related projects

- [touchHLE](https://github.com/touchHLE/touchHLE) — high-level iOS emulator with useful subsystem comparison points, although its architecture and target era differ
- [UWin](https://github.com/lujingyu/UWin) — historical 2011 iOS/Xcode build snapshot used only as external research evidence as described above

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