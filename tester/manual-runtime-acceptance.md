# Manual full IPA acceptance

This repository has a manual-only GitHub Actions acceptance path that deliberately uses the same public tester command used for local Windows testing:

```bat
Test-Ipa.cmd "C:\path\to\FrozenAcceptance.ipa" "C:\path\to\RuntimeRoot"
```

The workflow does not run on pushes, pull requests, or merges. A maintainer starts it explicitly from **Actions -> Manual full IPA acceptance -> Run workflow**.

## Purpose

The manual workflow lets maintainers test one frozen iOS IPA against the currently published Windows tester in `tester/windows/latest` without rebuilding the IPA. The same frozen IPA SHA can be reused indefinitely so changes in the observed boundary are attributable to ipaSim rather than to a newly compiled test application.

The workflow runs on GitHub-hosted `windows-2025` and invokes the unchanged `Test-Ipa.cmd` from the published tester ZIP. `Test-Ipa.cmd` continues to perform the same three stages as local testing: raw AArch64/Unicorn smoke, IPA extraction, and `IpaProbe` against the supplied RuntimeRoot.

## Frozen IPA configuration

Use a test IPA that is safe to make public and cache in this public repository's Actions cache. A small Swift/Foundation fixture is a good acceptance target.

The workflow accepts `ipa_url` and `ipa_sha256` when **Run workflow** is pressed. To avoid re-entering them on every run, configure these repository variables once:

- `IPASIM_ACCEPTANCE_IPA_URL` - immutable HTTPS URL for the frozen `.ipa`
- `IPASIM_ACCEPTANCE_IPA_SHA256` - lowercase or uppercase 64-character SHA256 of that exact file

If workflow inputs are supplied, they override the repository variables.

The IPA is cached by its immutable SHA256 and the hash is re-verified on every run, including cache hits.

## RuntimeRoot configuration

Do not commit an Apple RuntimeRoot to this public repository and do not cache it here in plaintext.

Create a password-encrypted 7z archive whose extracted contents expose either:

```text
RuntimeRoot\
  System\
  usr\
```

or directly:

```text
System\
usr\
```

For fast repeated extraction, a low-compression, non-solid encrypted archive is appropriate. From a machine with 7-Zip installed, one example is:

```bat
7z a -t7z -mx=1 -ms=off -mhe=on -pYOUR_PASSWORD RuntimeRoot.7z RuntimeRoot\
```

Keep that archive in private storage and configure these repository secrets:

- `IPASIM_RUNTIME_ARCHIVE_URL` - private or signed HTTPS URL for `RuntimeRoot.7z`
- `IPASIM_RUNTIME_ARCHIVE_SHA256` - SHA256 of the encrypted archive
- `IPASIM_RUNTIME_ARCHIVE_PASSWORD` - 7z password
- `IPASIM_RUNTIME_ARCHIVE_AUTH_HEADER` - optional full HTTP header such as `Authorization: Bearer ...`; leave unset for a signed/publicly reachable URL

The workflow caches only the encrypted `RuntimeRoot.7z`. The decrypted RuntimeRoot exists only under the ephemeral GitHub runner's temporary directory and is discarded with the VM.

The manual `runtime_cache_key` input defaults to `runtime-root-v1`. Bump it whenever the encrypted RuntimeRoot archive changes. The encrypted archive is still SHA-verified after a cache restore, so a stale or poisoned cache cannot silently be accepted.

## Published tester cache

The workflow always reads the current `tester/windows/latest/BUILD.txt` from `master` first. Its `zip_sha256` becomes the tester cache key. Therefore:

- a repeated run against the same published tester restores the already downloaded/extracted tester;
- a newly published tester automatically gets a different cache key;
- the tester ZIP SHA is verified after both downloads and cache restores.

No ipaSim compilation occurs in this workflow.

## Running on GitHub

1. Open the repository's **Actions** tab.
2. Select **Manual full IPA acceptance**.
3. Select **Run workflow**.
4. Leave the IPA fields empty when the repository variables are already configured, or supply an immutable IPA URL and SHA256.
5. Leave `runtime_cache_key` unchanged unless the RuntimeRoot archive changed.
6. Run the workflow.

The complete `Test-Ipa.cmd` console output is uploaded as a workflow artifact and the job summary records the test exit code, IPA/tester hashes, and cache-hit state.

## Running the identical test locally

Public contributors do not need the GitHub workflow to reproduce the execution path. Download/extract the published tester, supply their own legal RuntimeRoot, and run:

```bat
Test-Ipa.cmd "C:\path\to\FrozenAcceptance.ipa" "C:\path\to\RuntimeRoot"
```

The RuntimeRoot top level must contain `System` and `usr`.

The current tester is loader-focused. If it reports that application launch is not yet claimed, that is the current tester contract rather than a different behavior between local and GitHub testing.
