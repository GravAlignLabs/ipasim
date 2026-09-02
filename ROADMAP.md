# ipaSim runtime compatibility roadmap

This document records the **future semantic/runtime work** for the modern ARM64-on-Windows path. It complements `README.md` and `AGENTS.md`; it is not a promise to implement APIs in a fixed symbol-by-symbol order.

The project continues to use two different evidence loops:

1. **mechanical SDK/API/ABI coverage** is derived in bulk from Apple SDK metadata and compiler evidence; and
2. **semantic/runtime compatibility** advances from the first genuine non-cascading runtime boundary, implemented as a coherent subsystem with public regression coverage.

Generated ABI knowledge never grants semantic approval by itself.

## Current checkpoint

The latest merged compatibility increment is PR #65, which added XNU-style guarded regular-file open/close semantics after the generated `_read` migration in PR #64.

Current generated production routing includes the explicitly approved process-identity and descriptor-I/O set. APIs with untyped SDK records, variadic calling conventions, callbacks, data exports, or semantics that cannot be established mechanically remain outside generated production routing until their ABI and behavior are independently proven.

A separate active coordination claim owns the Darwin pthread-core increment. That work should be integrated before another contributor starts overlapping guest-thread lifecycle work.

## Architectural rule for future work

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

Every new subsystem must remain target-neutral and fail closed when a required semantic cannot be defended.

## Priority 0 — complete Darwin pthread core integration

Finish and merge the already-claimed pthread-core work before broadening the same lifecycle surface.

Required properties:

- exact Darwin LP64 `pthread_attr_t` layout and validation;
- stable guest-visible `pthread_t` identity;
- real create, return, exit, detach, and join lifetime behavior;
- independent ARM64 Unicorn execution contexts on Windows threads rather than synchronous re-entry of the requesting engine;
- shared guest process memory across those execution contexts;
- per-thread QoS, naming, signal-mask, and identity state where the current boundary requires it;
- executable semantic smokes for lifecycle and failure contracts;
- no regression to the existing workqueue, ulock, TSD, generated routing, descriptor, Mach IPC, or synthetic IPA baselines.

This is the current integration priority because later workqueue, cancellation, TSD teardown, and targeted ulock behavior depend on a trustworthy guest-thread identity/lifetime model.

## Priority 1 — Darwin event delivery: kqueue, kevent, workqueue, and workloop

The current workqueue bridge intentionally does not advertise KEVENT, WORKLOOP, or cooperative delivery modes, and the workloop bridge currently models control-plane lifecycle without event delivery.

Treat the missing functionality as one eventing subsystem rather than a collection of unrelated exports.

Target capabilities:

- guest-visible kqueue/workloop identity and lifetime;
- event registration and deregistration with exact structure/flag validation;
- readiness delivery for the first defensible Windows-backed event classes;
- integration with libdispatch workqueue callbacks;
- workloop/kevent callback delivery on independent guest-thread execution contexts;
- timeout and wake semantics that do not fabricate readiness;
- explicit rejection of unsupported filters or event classes;
- deterministic synthetic tests for registration, delivery, cancellation, timeout, and close races.

Do not advertise a workqueue feature bit merely because an export exists.

## Priority 2 — richer Mach IPC and voucher transport

The existing Mach IPC core provides useful in-process ports, rights counts, bounded queues, inline messages, and timeout behavior. Future compatibility requires expanding that model without turning Mach messages into opaque byte pipes.

Target capabilities:

- complex Mach message descriptors;
- send/receive right transfer with coherent namespace ownership;
- reply-right transfer;
- voucher-port transport through Mach messages;
- descriptor validation and failure codes before state mutation;
- receive-large/scatter behavior where required;
- message priority/QoS handling only when a defensible guest scheduling model exists;
- port lifetime notifications when real runtime evidence requires them.

The existing voucher callback bridge and Mach message transport must remain separate concepts: registering voucher callbacks is not equivalent to transporting voucher rights correctly.

## Priority 3 — complete pthread teardown and synchronization semantics

Once core pthread lifecycle is merged, complete the semantics that depend on it as coherent follow-on increments.

### TSD teardown

Implement guest-aware pthread-specific-data destructor execution rather than merely recording destructor pointers.

Required behavior should include:

- destructor invocation on guest pthread exit;
- Darwin-compatible repeated destructor passes when destructors repopulate keys;
- key deletion/lifetime correctness;
- no x64 execution of guest ARM64 callback addresses;
- deterministic teardown tests.

### Cancellation

Build a real guest pthread cancellation model before accepting cancellation-point flags in unrelated APIs.

Expected integration points include:

- per-thread cancellation state;
- deferred cancellation points;
- blocking waits;
- cleanup/teardown interaction;
- cancellation-safe state transitions.

### Ulock thread targeting and ownership

Extend ulock semantics only after Mach thread identity can select a real guest pthread.

Future work includes:

- targeted wake by Mach thread-port identity;
- owner validation tied to the actual guest thread namespace;
- priority-inheritance/turnstile behavior only where it can be represented honestly on the Windows-backed scheduler model.

Do not infer Darwin turnstile state from unrelated Windows thread priority data.

## Priority 4 — coherent Darwin signal subsystem

Do not implement individual waiting functions as isolated success shims.

A useful signal subsystem should define together:

- guest signal dispositions;
- per-thread signal masks;
- pending process/thread signals;
- mask updates;
- wait/interruption semantics;
- pthread cancellation interaction where Darwin defines a cancellation point;
- correct restart/interruption behavior for supported blocking APIs.

Until those semantics exist, unsupported signal waits should continue to fail explicitly rather than pretending a signal transition occurred.

## Priority 5 — commpage and CPU capability truth gate

Full-system iOS/XNU emulators reveal an important class of failure that ipaSim must prevent: guest userspace can read Apple CPU capability state and then select instructions or runtime paths that the execution engine cannot actually support.

ipaSim should eventually introduce a **CPU capability truth gate** for any Darwin commpage or equivalent processor-feature surface.

Core rule:

> Never advertise an Apple CPU capability unless the actual ARM64 execution engine has a passing executable proof for it.

The future capability model should distinguish at least:

- generic AArch64 baseline instructions;
- architectural atomics/LSE families;
- pointer-authentication instructions and keys;
- BTI behavior;
- Apple-specific system-register expectations;
- AMX or other Apple accelerator instructions;
- cache-line and CPU-topology properties exposed to userspace.

The gate should be generated or data-driven where possible, but capability advertisement must ultimately be backed by executable runtime tests rather than inferred from the host CPU or an iOS device model name.

A future pipeline could look like:

```text
candidate Apple CPU capability
          |
          v
execution-engine instruction probe
          |
    +-----+-----+
    |           |
   pass        fail
    |           |
    v           v
eligible      never advertise
for commpage  to guest userspace
    |
    v
synthetic guest capability test
```

Do not emulate a newer Apple CPU identity by exposing feature bits that Unicorn cannot execute.

## Priority 6 — filesystem, descriptor, and process-detail expansion

The descriptor namespace is now substantially more coherent, but future runtime evidence may require broader BSD/XNU behavior.

Likely subsystem work includes:

- additional `fcntl` commands with explicit Darwin-to-Windows translation;
- guarded descriptor capabilities beyond the currently supported guard set;
- vnode/path metadata and additional `proc_pidinfo` flavors;
- relative-path operations that consume the existing per-thread working-directory override correctly;
- filesystem metadata translation with exact Darwin field widths and timestamps;
- additional socket receive/send variants where address-result and ancillary-data semantics can be represented faithfully.

Variadic, callback-bearing, or untyped SDK interfaces remain outside generated production routing until their callable ABI is independently established.

## Priority 7 — Objective-C, Swift, framework, and higher runtime lifecycle

Once the lower Darwin runtime is stable enough to carry modern libSystem/libdispatch behavior, expand higher-level runtime compatibility from public framework fixtures.

Potential boundaries include:

- Objective-C runtime lifecycle and metadata expectations not already handled by the historical bridge;
- blocks/callback ownership and escaping callback lifetimes;
- Swift runtime dependencies;
- Foundation lifecycle and run-loop behavior;
- UIKit/application lifecycle only when lower runtime prerequisites are real.

Higher framework progress must not be achieved by hiding a lower-level Darwin failure.

## Explicit non-goals inherited from full-system emulators

Full-system XNU emulators are valuable behavioral references, but many of their patches exist only because they boot a complete Apple kernel and root filesystem. Those patches are **not** ipaSim implementation requirements.

Unless ipaSim's architecture changes to boot XNU, do not add compatibility work whose sole purpose is:

- APFS root authentication, snapshot, or root-filesystem patching;
- AMFI/code-signing/trust-cache bypasses;
- kernel `pmap` code-sign enforcement bypasses;
- IMG4/firmware-signature bypasses;
- SEP firmware emulation or activation bypasses;
- Apple interrupt-controller, DART, SART, or other SoC-device emulation;
- jailbreak-style privilege or mount changes.

ipaSim should model the userspace contract it actually exposes, not reproduce security bypasses needed by a full-system virtual machine.

## Regression contract for every roadmap increment

A roadmap item is not complete merely because the next application failure moves.

Every compatibility PR should preserve the known-good public boundaries and include the smallest focused proof for its new behavior.

Minimum acceptance rules:

- no application-specific names, bundle identifiers, paths, fingerprints, or private logs in public history;
- no monkey patching or hidden compatibility hooks;
- no fabricated success for unsupported Darwin behavior;
- complete guest pointer/span validation before host dereference when the API requires it;
- exact Darwin LP64 structure widths and return/error conventions;
- prior generated semantic routes remain green;
- prior descriptor, Mach IPC, pthread/workqueue, VM, timing, socket, and filesystem smokes remain green when applicable;
- `HelloBootstrap.ipa` continues to execute its known ARM64 result;
- untouched native and UIKit synthetic fixtures continue to reach their documented dependency/runtime-root boundaries until those boundaries are intentionally replaced by a real implementation;
- CI publishes an actionable failure diagnostic and then fails normally; it must never hide a compiler, linker, packaging, semantic-test, or runtime failure.

## How the next implementation is selected

The roadmap is ordered by architectural dependency and expected modern-runtime pressure, but it does **not** override runtime evidence.

After each independently correct increment merges:

1. start from updated `master`;
2. inspect active claims and open/draft PRs;
3. run the applicable public validation first;
4. use real acceptance execution only after public baselines are understood;
5. identify the first genuine non-cascading semantic/runtime boundary;
6. map that boundary onto the coherent subsystem above;
7. create a narrow claim;
8. implement the smallest complete subsystem increment that fixes the abstraction rather than only the observed symbol;
9. add public semantic regression coverage;
10. merge when independently correct and green, then repeat.

This preserves the project's central goal: progressively broader modern iOS compatibility on Windows without turning the emulator into a collection of target-specific exceptions.
