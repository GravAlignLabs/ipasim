# GitHub Copilot Instructions for ipaSim

The canonical repository instructions are in [`/AGENTS.md`](../AGENTS.md). Read and follow that file before changing code.

When working under `src/IpaSimulator/`, also follow [`/src/IpaSimulator/AGENTS.md`](../src/IpaSimulator/AGENTS.md).

Critical rules, repeated here so they remain visible even when a Copilot surface does not automatically traverse referenced instruction files:

- Keep public work target-neutral and use repository-generated synthetic fixtures for reproduction.
- Do not add private application names, paths, bundle identifiers, logs, fingerprints, screenshots, or binary-specific hacks.
- Do not monkey patch, runtime-swap methods, add hidden compatibility hooks, or turn unsupported Darwin/iOS behavior into fake success.
- Prefer real Windows-backed semantics when a defensible mapping exists; otherwise fail explicitly with a useful diagnostic.
- Work from the first genuine non-cascading failure and fix the responsible subsystem rather than bypassing it.
- Preserve existing known-good ARM64 behavior and semantic smoke coverage.
- Run public validation in this order: `synthetic-hello-ipa.yml`, then `windows-arm64-core.yml`.
- Preserve the PR diagnostic contract: publish/update the useful diagnostic, then allow the workflow to fail normally. Never suppress a red check to make diagnostic reporting succeed.
- For ARM64 host-call work, validate AAPCS64 signatures, argument widths/counts, return behavior, and data-vs-code exports before wiring `SysTranslator`.

If these instructions conflict with a more specific `AGENTS.md` in the edited subtree, follow the more specific instructions for that subtree while still respecting repository-wide privacy and no-fake-success rules.
