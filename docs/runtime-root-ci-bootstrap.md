# GitHub-hosted RuntimeRoot CI supply

ipaSim's trusted public-IPA acceptance does not require a maintainer to package,
host, or upload a local RuntimeRoot. GitHub-hosted macOS runners already contain
Apple's iOS simulator runtime and SDKs, so trusted CI uses that installed material
as its source.

The Windows ipaSim job still receives only an encrypted RuntimeRoot archive. This
is intentional because default-branch Actions caches can be restored by
lower-trust workflows. Plaintext RuntimeRoot files exist only in ephemeral runner
storage.

## Pinned GitHub source

The trusted source job currently requires:

- runner image: `macos-15`;
- Xcode: `16.4`;
- iOS simulator runtime: `18.5`;
- runtime build: `22F77`.

The job selects `/Applications/Xcode_16.4.app`, queries `simctl` JSON for runtime
build `22F77`, and uses the returned `.simruntime` bundle path. It then requires:

```text
<bundle>.simruntime/Contents/Resources/RuntimeRoot/System
<bundle>.simruntime/Contents/Resources/RuntimeRoot/usr
```

No private/local filesystem path is assumed. If GitHub removes or changes the
pinned runtime, the source job fails with an explicit diagnostic instead of
silently selecting another runtime.

## One small secret, no local archive

The only repository secret required for this GitHub-native runtime path is:

```text
IPASIM_RUNTIME_ARCHIVE_PASSWORD
```

It is used automatically by the trusted macOS job to encrypt `RuntimeRoot.7z`
and by the trusted Windows job to decrypt it. The password is not needed
interactively during normal CI.

A maintainer can create it once from an authenticated GitHub CLI session without
creating or touching a RuntimeRoot archive locally:

```powershell
$bytes = New-Object byte[] 32
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($bytes)
$password = -join ($bytes | ForEach-Object { $_.ToString('x2') })
$password | gh secret set IPASIM_RUNTIME_ARCHIVE_PASSWORD --repo GravAlignLabs/ipasim --app actions
[Array]::Clear($bytes, 0, $bytes.Length)
$password = $null
$rng.Dispose()
```

The old URL, archive-SHA, and authorization-header secrets are not required by
this workflow because GitHub itself supplies and caches the runtime.

## Workflow separation

`public-ipa-acceptance.yml` is secret-free. It keeps the frozen no-RuntimeRoot AWS
baseline available on pull requests and trusted `master` pushes.

`github-runtime-source-preflight.yml` is also secret-free. It runs on pull
requests that change this runtime-supply infrastructure and proves that GitHub's
macOS image still exposes the pinned Xcode, iOS simulator runtime, `RuntimeRoot`,
and SDK identities.

`trusted-github-runtime-acceptance.yml` is master-push only. It is the only new
workflow that references `IPASIM_RUNTIME_ARCHIVE_PASSWORD`.

## Trusted runtime flow

On a trusted `master` push:

1. a GitHub-hosted macOS runner selects the pinned Xcode;
2. it inventories the installed `iphoneos` and `iphonesimulator` SDK identities;
3. it locates iOS simulator runtime build `22F77` through `simctl`;
4. it validates the runtime's `System` and `usr` directories;
5. it restores the encrypted cross-OS runtime cache when present;
6. on cache miss, it creates `RuntimeRoot.7z` directly from the installed
   simulator RuntimeRoot using 7-Zip header encryption;
7. it hashes the encrypted archive and publishes only the hash/runtime identity
   as job outputs;
8. it saves only the encrypted archive to Actions cache;
9. the trusted Windows job restores that exact cache entry and verifies its hash;
10. Windows decrypts the RuntimeRoot into `runner.temp`;
11. the pinned AWS IPA runs through the exact published
    `Test-Ipa.cmd <IPA> <RuntimeRoot>` path;
12. the GitHub-hosted runners are discarded.

There is no local 11 GB compression step, external RuntimeRoot hosting, or
plaintext RuntimeRoot cache.

## Pull-request isolation

Pull requests continue to run the frozen AWS loader acceptance without any
RuntimeRoot or RuntimeRoot secret. The secret-bearing GitHub-hosted runtime
source/cache path does not run from pull-request code.

The separate runtime-source preflight may also be dispatched manually because it
contains no secret references. It only selects Xcode, locates the installed
runtime, validates `System`/`usr`, and inventories SDK identities. It does not
create an archive or save a RuntimeRoot cache.

## SDK inventory

The same macOS runner also exposes Apple SDKs. This PR records these identities
for future work using:

```text
xcrun --sdk iphoneos --show-sdk-path
xcrun --sdk iphoneos --show-sdk-version
xcrun --sdk iphonesimulator --show-sdk-path
xcrun --sdk iphonesimulator --show-sdk-version
```

SDK contents are not cached or consumed by ipaSim in this change. A later PR can
use the observed paths/versions to decide whether GitHub-hosted SDKs should
replace other SDK acquisition paths.

## Security and compatibility rules

- Never commit RuntimeRoot or SDK binaries to ipaSim.
- Never cache or artifact-upload plaintext RuntimeRoot files.
- Never expose the trusted runtime job to pull-request code.
- Do not turn missing runtime capabilities into aliases, monkey patches, or
  hidden-success paths.
- Keep the frozen public AWS relocation -> AVFoundation -> loader-stop baseline
  independent from trusted full-runtime acceptance.
- Runtime progress must remain target-neutral and regression-tested.
