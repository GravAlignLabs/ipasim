# IpaSimulator Core Agent Instructions

These instructions apply to `src/IpaSimulator/` and supplement the repository-root `AGENTS.md`.

## Core responsibility

This directory contains the modern emulator core and the compatibility boundaries most likely to affect ARM64 iOS execution on Windows.

Treat failures here as subsystem/ABI problems first, not as application-specific problems.

## Key components

Use the existing file responsibilities rather than creating duplicate pathways:

- `ModernMachO.cpp` / `ModernMachO.hpp` — modern ARM64 Mach-O parsing/model
- `DynamicLoader.cpp` / `DynamicLoader.hpp` — image loading, dependency resolution, dyld-facing behavior
- `SysTranslator.cpp` / `SysTranslator.hpp` — guest-to-host call translation and ARM64 ABI marshalling
- `MachIpc.cpp` / `MachIpc.hpp` — in-process Mach messaging/port compatibility
- `DarwinHostBridge.cpp` / `.def` — exported Darwin host surface
- `DarwinHostCompatBridge.cpp` — simulator-host compatibility boundaries that do not belong in narrower adapters
- `DarwinKernelBridge.cpp` — process, VM, kernel-oriented Darwin compatibility
- `DarwinSocketAdapter.cpp` / `.hpp` and `DarwinSocketBridge.cpp` — Winsock-backed socket semantics
- `DarwinTimeBridge.cpp` — Darwin timing behavior
- `FifoAdapter.cpp` / `.hpp` — FIFO-specific filesystem semantics
- `HostImportInventory*.hpp` — host-facing import inventory/gap tracking
- `IpaProbe.cpp` — loader/execution probe used by generic IPA testing
- `*Smoke.cpp` — semantic verification; these are executable specifications, not disposable test scaffolding

Before adding a new bridge file, verify that the behavior does not already belong in one of these components.

## ARM64 ABI rules

- Guest code follows AAPCS64.
- Integer/pointer arguments begin in `X0` through `X7`; additional arguments use the guest stack according to the ABI.
- Return values normally flow through `X0` unless the specific ABI contract says otherwise.
- Never guess a host-call signature from symbol spelling alone. Confirm argument count, widths, signedness where relevant, return behavior, and whether the symbol is data or code.
- A host export existing in `IpaSimDarwinHost.dll` is not sufficient; `SysTranslator` must know how to marshal calls that the guest will actually execute.
- Exported data such as Darwin globals must not be registered as callable host functions.
- Preserve 64-bit guest addresses; do not reintroduce historical 32-bit truncation assumptions.

## Mach-O and dyld rules

- Preserve Mach-O load-command and ordinal semantics rather than normalizing away information needed later by dyld resolution.
- Treat chained fixups, export tries, dependency ordinals, rpaths, and RuntimeRoot resolution as loader semantics, not per-application hacks.
- When a dependency or symbol cannot be resolved, report the exact image/symbol/boundary that failed.
- Do not silently redirect arbitrary Apple libraries to the Windows host bridge.
- Synthetic bootstrap fixtures may intentionally isolate the execution path, but production loader behavior must remain honest about unresolved Apple runtime/framework dependencies.

## Darwin-to-Windows translation rules

A translation is acceptable when it preserves the property the Darwin caller relies on, even if the underlying Windows mechanism differs.

Examples of appropriate approaches:

- monotonic/sleep-inclusive host timing for Darwin continuous-time semantics, paired with a coherent timebase
- Winsock for Darwin socket behavior with explicit address/flag translation
- Windows process APIs for process identity and measurable task statistics
- `VirtualAlloc`/`VirtualFree` for VM behavior when allocation tracking preserves the Darwin contract

Examples of inappropriate approaches:

- returning success while doing nothing
- inventing process IDs/groups/flags
- returning a resolved Windows path where Darwin requires symlink-descriptor semantics
- treating Authenticode metadata as if it were Darwin code-signing flags or entitlements
- ignoring deallocation size/region identity when Darwin semantics require it

If the semantic gap cannot yet be closed, fail explicitly and leave a diagnostic that identifies the unsupported behavior.

## Guest descriptor model

Descriptor behavior must be coherent across files, FIFOs, and sockets.

- Do not probe arbitrary UCRT descriptor numbers to discover open descriptors; invalid CRT descriptors can enter Windows invalid-parameter handling and terminate the process.
- Track guest-visible descriptors explicitly when enumeration/lifetime/type information is required.
- `close`, `read`, `write`, `fcntl`-style behavior, and `proc_pidinfo(PROC_PIDLISTFDS)` should observe the same guest descriptor model.
- Route socket descriptors through Winsock-backed operations rather than UCRT file I/O.
- Preserve sizing-query behavior where Darwin APIs allow callers to query required buffer length before providing storage.

Prefer moving toward one coherent `DarwinDescriptorTable` abstraction over accumulating independent descriptor registries if future work demonstrates that the split is causing repeated cross-subsystem fixes.

## VM rules

- Track reservations when Windows release semantics cannot reproduce Darwin VM lifetime/size rules from an address alone.
- Do not silently accept partial deallocation if the current implementation can only release a complete tracked reservation.
- Reject unsupported address/flag combinations explicitly.
- Keep guest page-size exports consistent with the host-backed allocation model used by the emulator.

## Time rules

- Preserve monotonicity and the sleep/boot-time property expected by the Darwin API being implemented.
- Keep `mach_continuous_time` and any exposed timebase coherent with each other.
- Do not substitute a lower-fidelity clock solely to make linking easier when a suitable Windows API exists.

## Mach IPC rules

- Keep guest Mach ports/tasks as guest concepts with explicit mapping to in-process emulator objects.
- Validate packed `mach_msg2` fields according to the Darwin/XNU ABI before forwarding to older message helpers.
- Reject unsupported vector/descriptor paths explicitly until implemented.
- Do not let Windows thread/process handles leak directly into guest Mach identity.

## pthread/workgroup rules

- Darwin `pthread_t` and workgroup objects are guest abstractions, not Windows thread handles.
- Do not convert unsupported workgroup installation/create APIs into no-op success.
- Proper workgroup support may require storing guest callback tables and invoking ARM64 guest callbacks through a general callback path; implement that architecture rather than hard-coding a one-off callback.

## Smoke-test rules

Every new semantic implementation should have a focused smoke assertion when practical.

A smoke test should:

- call the same public/exported boundary the guest relies on where possible
- verify both success semantics and important failure semantics
- avoid undefined/invalid host operations merely to inspect state
- print enough stage information that CI identifies the last completed semantic checkpoint
- return nonzero on failure

Do not weaken a smoke test to make CI green. Fix the implementation or correct an invalid test assumption with evidence.

## Debugging order

When a new target or synthetic fixture advances:

1. identify the first non-cascading missing symbol or semantic failure
2. check whether the symbol is already exported but missing a `SysTranslator` signature
3. verify ABI/signature/data-vs-function classification
4. determine which existing subsystem owns the behavior
5. implement the smallest correct general semantic change
6. add/extend smoke coverage
7. rerun synthetic IPA CI
8. rerun Windows ARM64 Core

Do not implement later cascading failures before the first genuine boundary is understood.

## Comparing other emulators

Use touchHLE and similar projects to accelerate architecture decisions, not to import incompatible assumptions.

High-value comparison areas include:

- unified guest descriptor tables
- guest object identity/lifetime management
- syscall/API semantic modeling
- Mach/time abstractions
- framework boundary organization

Always reconcile those designs with modern ARM64, AAPCS64, current Mach-O/dyld formats, and the Windows host model used here.
