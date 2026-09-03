# AWS Device Farm sample iOS app

This public acceptance workload is an independent third-party IPA used to exercise
ipaSim's real IPA packaging, Mach-O parsing, load-command handling, and dependency
boundary behavior without relying on private application data.

## Provenance

- Upstream repository: `aws-samples/aws-device-farm-sample-app-for-ios`
- Upstream commit: `58e48234db510bd4fbf643643e8808c5d6a13845`
- Upstream path: `prebuilt/prebuiltSampleApp.ipa`
- Git blob SHA-1: `06a33a39286ffd7c9d300c5924750b6f97c4e346`
- Upstream file size: 2,138,167 bytes
- License: Apache License 2.0 (`LICENSE.txt` in the upstream repository)

The ipaSim repository does not need to duplicate the binary. The public acceptance
workflow downloads the file from the immutable upstream commit and verifies its
Git blob identity with `git hash-object` before executing it. This keeps the test
reproducible without adding another multi-megabyte binary to ipaSim history.

The application is intentionally independent of ipaSim. AWS describes it as a
native iOS reference application containing many stock iOS components and uses it
as a Device Farm test workload.

## Acceptance contract

The public test never supplies proprietary Apple RuntimeRoot material. Therefore a
normal loader stop at the first Apple framework/runtime dependency is expected
until ipaSim has a public semantic replacement for that boundary.

The first workflow run is a discovery checkpoint. It requires:

1. the downloaded IPA to match the pinned upstream Git blob exactly;
2. the committed published ipaSim tester to pass its SHA-256 provenance check;
3. `IpaProbe` not to crash while processing the real third-party ARM iOS package;
4. any nonzero probe result to contain an explicit ipaSim loader-stop diagnostic.

After the first successful discovery run, the exact first non-cascading loader
boundary is pinned in the workflow and becomes a regression requirement, in the
same style as the existing synthetic native and UIKit IPA fixtures.

This fixture must not be used as justification for application-specific aliases,
monkey patches, hidden success paths, or redistribution of Apple runtime files.
