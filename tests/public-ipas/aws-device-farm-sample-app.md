# AWS Device Farm sample iOS app

This public acceptance workload is an independent third-party IPA used to exercise
ipaSim's real IPA packaging, Mach-O parsing, load-command handling, dependency
boundary behavior, and trusted full-runtime execution without relying on private
application data.

## Provenance

- Upstream repository: `aws-samples/aws-device-farm-sample-app-for-ios`
- Upstream commit: `58e48234db510bd4fbf643643e8808c5d6a13845`
- Upstream path: `prebuilt/prebuiltSampleApp.ipa`
- Git blob SHA-1: `06a33a39286ffd7c9d300c5924750b6f97c4e346`
- Upstream file size: 2,138,167 bytes
- License: Apache License 2.0 (`LICENSE.txt` in the upstream repository)

The ipaSim repository does not duplicate the binary. The acceptance workflow
downloads the file from the immutable upstream commit and verifies its Git blob
identity with `git hash-object` before executing it. This keeps the test
reproducible without adding another multi-megabyte binary to ipaSim history.

The application is intentionally independent of ipaSim. AWS describes it as a
native iOS reference application containing many stock iOS components and uses it
as a Device Farm test workload.

## Two acceptance modes

### Public pull-request loader check

Every pull-request execution is deliberately unprivileged. It receives no
RuntimeRoot secrets and runs the untouched AWS application image through the
published `IpaProbe` tester. This proves that a real third-party package is parsed
without crashing and that unsupported loader behavior remains explicit rather
than becoming hidden success.

The public check requires:

1. the downloaded IPA to match the pinned upstream Git blob exactly;
2. the committed published ipaSim tester to pass its SHA-256 provenance check;
3. `IpaProbe` not to crash while processing the real third-party ARM iOS package;
4. the exact frozen loader observations below to remain visible until target-neutral
   compatibility work legitimately advances them.

#### Frozen public baseline

The discovery run established this exact current sequence:

```text
first_error=Error: unsupported ARM64 relocation.
runtime_boundary=Error: iOS runtime root is not configured for dependency /System/Library/Frameworks/AVFoundation.framework/AVFoundation.
loader_stop=[ipasim-probe] loader stopped with code 2 before app execution.
probe_exit=2
```

The relocation diagnostics occur before the missing-`AVFoundation` RuntimeRoot
boundary and are therefore part of the compatibility baseline, not log noise. The
workflow must not suppress or skip them. A future relocation implementation may
advance this checkpoint only by replacing the unsupported behavior with real,
target-neutral loader semantics and updating the frozen expectation from observed
CI evidence.

The missing `AVFoundation` dependency is separately recorded because the public PR
job intentionally has no Apple RuntimeRoot. The explicit loader-stop diagnostic
and exit code 2 prove that the unsupported dependency terminates through ipaSim's
normal loader failure path rather than a process crash.

### Trusted full-runtime acceptance

A second job runs only for trusted `master` pushes or a manual workflow dispatch
from `master`. It never runs for `pull_request` events.

This job reuses the repository's existing encrypted RuntimeRoot cache contract:

1. restore only `RuntimeRoot.7z` from the `ipasim-encrypted-runtime-*` Actions
   cache;
2. on cache miss, download the encrypted archive from the configured repository
   secret location;
3. verify the encrypted archive against `IPASIM_RUNTIME_ARCHIVE_SHA256`;
4. decrypt it with `IPASIM_RUNTIME_ARCHIVE_PASSWORD` into `runner.temp` only;
5. validate that the ephemeral root exposes `System` and `usr`;
6. run the untouched pinned AWS IPA through the exact published
   `Test-Ipa.cmd <IPA> <RuntimeRoot>` path;
7. upload only the console log and public tester provenance.

The plaintext RuntimeRoot is never committed, never saved to Actions cache, and
never uploaded as an artifact. It exists only on the ephemeral GitHub-hosted
Windows runner for the lifetime of the trusted job. The encrypted archive uses
the same cache namespace and repository secrets as the existing manual full-IPA
acceptance workflow.

## Security and compatibility rules

The trusted job is intentionally unavailable to pull requests so unreviewed PR
code cannot access RuntimeRoot secrets or plaintext extracted files. Manual
execution is also restricted to the `master` ref.

This fixture must not be used as justification for application-specific aliases,
monkey patches, hidden success paths, failure suppression, or
committing/redistributing Apple runtime files. Runtime progress must come from
target-neutral ipaSim compatibility work.
