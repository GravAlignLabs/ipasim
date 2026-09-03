# RuntimeRoot CI bootstrap

ipaSim's trusted GitHub Actions acceptance jobs can use an encrypted RuntimeRoot
without requiring anyone to type the archive password during normal CI runs.

The password is generated once on the Windows machine that owns the RuntimeRoot,
then sent directly to the repository's GitHub Actions secret store through the
authenticated GitHub CLI. The generated password is not committed and the helper
does not print it.

## Prerequisites

- A local RuntimeRoot containing top-level `System` and `usr` directories.
- 7-Zip installed.
- GitHub CLI (`gh`) installed and authenticated to an account with permission to
  manage Actions secrets in `GravAlignLabs/ipasim`.

Authenticate once if needed:

```powershell
gh auth login
```

## One-time setup

From an ipaSim checkout, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Prepare-RuntimeRootArchive.ps1 `
  -RuntimeRoot "C:\path\to\RuntimeRoot" `
  -ArchiveUrl "https://example.invalid/private/RuntimeRoot.7z"
```

The helper performs these operations in order:

1. validates that the RuntimeRoot contains `System` and `usr`;
2. checks that `gh` is authenticated and that 7-Zip is available;
3. refuses to overwrite an existing archive or rotate an existing RuntimeRoot
   password secret unless `-Rotate` is explicitly supplied;
4. generates 32 cryptographically random bytes and represents them as a 64-digit
   hexadecimal password;
5. creates `RuntimeRoot.7z` with 7-Zip header encryption (`-mhe=on`);
6. computes SHA-256 and writes a non-secret local `RuntimeRoot.7z.sha256` file;
7. sends the password to `IPASIM_RUNTIME_ARCHIVE_PASSWORD` using `gh secret set`
   over standard input;
8. sends the archive hash to `IPASIM_RUNTIME_ARCHIVE_SHA256`;
9. when `-ArchiveUrl` is supplied, sets `IPASIM_RUNTIME_ARCHIVE_URL` as well;
10. when `-ArchiveAuthHeader` is supplied, sets the optional
    `IPASIM_RUNTIME_ARCHIVE_AUTH_HEADER` secret.

The password is intentionally not displayed after generation. GitHub Actions uses
it automatically on future trusted runs.

## Archive hosting and the first cache fill

The current trusted workflow caches only the encrypted `RuntimeRoot.7z`. A fresh
GitHub Actions cache still needs a source URL for the first download. The archive
URL may be omitted from the helper when preparing the archive, but
`IPASIM_RUNTIME_ARCHIVE_URL` must exist before the first cache-miss run.

After the encrypted archive has been cached, later runs restore it from Actions
cache and normally do not use the source URL. They still require the password and
SHA-256 secrets so the archive can be verified and decrypted on the ephemeral
runner.

Plaintext RuntimeRoot files are extracted only into `runner.temp`; they are not
placed in Git, Actions cache, or uploaded artifacts.

## Intentional rotation

Do not rotate the password independently of the encrypted archive. To replace the
RuntimeRoot or password, generate a matching new pair deliberately:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Prepare-RuntimeRootArchive.ps1 `
  -RuntimeRoot "C:\path\to\RuntimeRoot" `
  -ArchiveUrl "https://example.invalid/private/RuntimeRoot.7z" `
  -Rotate
```

Then bump the workflow's `runtime_cache_key` when running the trusted acceptance
job so GitHub does not restore an archive encrypted with the previous password.
