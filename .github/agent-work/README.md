# Agent work claims

This directory is the lightweight coordination surface for autonomous agents and trusted maintainers working on ipaSim.

The purpose is simple: before an agent disappears into a feature branch, `master` should contain a small machine-readable record saying what boundary is being worked on.

## Why claims live on `master`

Feature branches are not a reliable coordination surface when several agents are working at once. A tiny metadata-only claim on `master` lets every agent start from the same answer to: **what is somebody already working on?**

Claims are coordination metadata only. They are not permission to put unfinished source code directly on `master`.

## Claim format

Create one YAML file per independently scoped work item:

```text
.github/agent-work/<short-scope-slug>.yml
```

Copy `CLAIM_TEMPLATE.yml` and fill in every field. Keep the task narrow enough that another agent can tell whether its planned work overlaps.

Example filename:

```text
.github/agent-work/darwin-descriptor-table.yml
```

## Required sequence for agents with trusted write access

1. Update your local view of `master`.
2. Read every active `*.yml` claim in this directory.
3. Check open/draft PRs for overlapping work.
4. If the intended boundary is already actively claimed, coordinate or choose another boundary instead of duplicating the work.
5. Add one claim file to `master` in a **metadata-only** commit such as `claim: darwin descriptor table`.
6. Create the feature branch **from that updated `master`**, so the branch includes its own claim.
7. Do the real implementation and testing on the feature branch.
8. Open a focused PR as soon as the increment is coherent.
9. Delete the claim file in the implementation PR so the claim disappears when the work lands.
10. Start follow-on work from the newly updated `master` and create a new claim for the next independently mergeable boundary.

A claim commit must not include source-code changes. Direct-to-`master` commits are reserved here for coordination metadata, not implementation shortcuts.

## Heartbeats and stale claims

If work lasts more than 72 hours, update the claim's `updated_utc` and `next_checkpoint` in a small metadata-only commit such as:

```text
claim update: darwin descriptor table
```

A claim is considered **stale after 7 days without an update**. Stale claims must not reserve a subsystem indefinitely.

Before taking over work from a stale claim, check whether there is an active PR or recent branch activity. Maintainers may remove stale claims after confirming the work is no longer active.

If work is abandoned intentionally, delete the claim with a small commit such as:

```text
release claim: darwin descriptor table
```

## Contributors without direct `master` write access

Public contributors should not be granted write access merely to create claims. If branch protection or repository permissions prevent a direct metadata claim:

1. check the active claim files first;
2. open a draft PR immediately for the intended narrow boundary;
3. identify the scope clearly in the PR title/body;
4. treat that draft PR as the public work claim until a maintainer decides whether a claim file should be landed on `master`.

Agents must check both this directory and open/draft PRs before beginning substantial work.

## Privacy

Claims are public. Never put private IPA names, bundle identifiers, local paths, RuntimeRoot contents, private logs, screenshots, binary fingerprints, or other private acceptance data in a claim.

Describe the emulator boundary generically, for example `Darwin descriptor table`, `mach_msg2 ABI`, or `UIKit dependency probe`.

## Scope quality

Good claim:

```text
Implement coherent guest descriptor routing for close/write/fcntl and PROC_PIDLISTFDS
```

Bad claim:

```text
Finish iOS support
```

Claims should describe one independently mergeable engineering increment, consistent with the repository's rolling-integration policy.
