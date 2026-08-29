## Compatibility boundary

Describe the specific loader, ABI, Darwin/Windows semantic, runtime, test, or CI boundary this PR advances.

## What this increment delivers

Explain the independently useful behavior that is ready to return to `master` now. Do not make completion of the entire emulator, framework stack, or application compatibility effort a prerequisite for merging this increment.

## Validation

- [ ] The change has a focused public reproduction or semantic test where practical.
- [ ] **Synthetic iOS IPA on Windows** has been run or is not affected by this change.
- [ ] **Windows ARM64 Core** has been run or is not affected by this change.
- [ ] Existing known-good ARM64 behavior is preserved.
- [ ] Unsupported behavior remains explicit; this PR does not introduce fake-success shims or hidden fallbacks.
- [ ] No private application names, paths, bundle identifiers, RuntimeRoot contents, private logs, screenshots, or binary-specific hacks are included.

## Integration checkpoint

- [ ] This PR is a coherent, independently reviewable increment rather than a branch waiting for the whole project to be finished.
- [ ] Work that is complete and tested in this PR can merge to `master` now.
- [ ] Unfinished adjacent work is identified as follow-up work and will continue from the updated `master` in another PR rather than keeping this PR open indefinitely.
- [ ] If this PR has grown across multiple independent subsystems, it has been split where practical.

## Follow-up work

List the next genuine compatibility boundaries separately. Merging this PR should not imply that those future boundaries are already implemented.
