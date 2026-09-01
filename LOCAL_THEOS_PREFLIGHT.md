# Local Theos SDK compatibility preflight

This is the developer-machine mirror of `.github/workflows/theos-sdk-preflight.yml`.
It exists so the expensive SDK compatibility work can be debugged and repeated
locally while GitHub Actions remains the clean independent verifier.

## Parity marker

The checked-in parity record is
`tools/compat_surface/local_theos_preflight_baseline.json`.

**Last synchronized CI point:**

- PR: `#58`
- source head: `ca4a2e71b997b55d0176acb21ebdda55fd928fbd`
- Theos SDK Preflight: **run #37** (`33534466454`), queued when the marker was written
- Compatibility Surface Analyzer: **run #85** (`33534466472`), completed successfully
- workflow blob: `0ec88734c8b7ca85935488e031c19635021d639e`
- pinned SDK: `theos/sdks@0222fd5413cf4b9af096f37b4621afa2688572f7`
- SDK: `iPhoneOS16.5.sdk`

The local runner computes the current Git blob SHA for the CI workflow every
time it starts. If that SHA differs from the baseline above, it prints a
prominent **CI workflow parity: DRIFT** warning. That is the signal to compare
the workflow and local runner before relying on local/CI parity, then update the
baseline record to the new run that was used for synchronization.

Analyzer source changes do not silently reuse old results. Header shards are
stored under a content-addressed cache key built from the header analyzer source,
the pinned SDK, the target, and the local Clang identity. Downstream bundles use
a broader compatibility-tool fingerprint.

## Windows one-click entrypoint

From File Explorer at the repository root, double-click:

```text
Run-Theos-Preflight.bat
```

The batch file launches the pipeline inside WSL/Ubuntu. On the first run it
creates `out/local-theos-preflight/venv`, installs the pinned `PyYAML==6.0.2`, creates a
sparse checkout of the pinned Theos SDK, runs the fast compatibility tests, runs
or reuses all 32 exhaustive header shards, then runs the complete SDK
compatibility pipeline.

Required WSL packages:

```bash
sudo apt update
sudo apt install -y python3 python3-venv git clang
```

All downloaded SDK data, virtual environments, shard manifests, logs, cache
metadata, and generated compatibility bundles live below
`out/local-theos-preflight/`. The repository already ignores `/out/`, so local
preflight evidence cannot accidentally become part of a normal commit.

## What the full local run mirrors

The default `full` command mirrors the substantive CI computation:

```text
pinned iPhoneOS16.5.sdk
        -> Compatibility Surface Analyzer tests
        -> 32 deterministic exhaustive header shards
        -> exact shard coverage/manifest merge
        -> complete TAPI scan
        -> SDK typed catalog
        -> AAPCS64 lowering
        -> Win64 lowering
        -> libffi bridge plans
        -> runtime adapter table
        -> compatibility planner
        -> explicit semantic routes
        -> generated compatibility bundle
```

CI-specific PR comments, GitHub step summaries, artifact uploads, runner matrix
scheduling, and the deliberate final red diagnostic gate are not reproduced
locally.

## Incremental commands

Use the WSL shell wrapper when you want something smaller than the default
double-click full run:

```bash
# Show the parity marker and reusable cache
./tools/compat_surface/run_theos_preflight.sh status

# Only prepare/update the pinned sparse SDK checkout
./tools/compat_surface/run_theos_preflight.sh prepare

# Run only the fast compatibility tests
./tools/compat_surface/run_theos_preflight.sh tests

# Run one problem shard
./tools/compat_surface/run_theos_preflight.sh shards --shard 18

# Run several selected shards
./tools/compat_surface/run_theos_preflight.sh shards --shard 7 --shard 13 --shard 27 --shard 28

# Run/reuse all shards without the downstream pipeline
./tools/compat_surface/run_theos_preflight.sh shards

# Run downstream analysis from a complete current shard cache
./tools/compat_surface/run_theos_preflight.sh downstream

# Full validation, but allow more local shard concurrency
./tools/compat_surface/run_theos_preflight.sh full --parallel-shards 8

# Deliberately discard current analysis cache and rerun it
./tools/compat_surface/run_theos_preflight.sh clean
./tools/compat_surface/run_theos_preflight.sh full --force-shards --force-downstream
```

The conservative default is four simultaneous shard processes with two Clang
header workers per shard. This uses eight header-analysis workers instead of
copying CI's much more aggressive runner matrix directly. Increase
`--parallel-shards` only when local CPU and memory capacity justify it.

## Cache and recovery behavior

A successful shard is kept as:

```text
out/local-theos-preflight/
  cache/<header-fingerprint>/header-shards/
    header-shard-0.json
    header-shard-0.log
    header-shard-0.status
    ...
```

If a run is interrupted, successful manifests remain reusable. A later `full`
run executes only missing/invalid shards for the current fingerprint. It never
mixes manifests from different analyzer fingerprints.

A successful complete pipeline is kept under:

```text
out/local-theos-preflight/output/<pipeline-fingerprint>/bundle/
```

`latest.json` records the most recent local invocation, repo head, cache keys,
SDK commit, bundle location, and the checked-in CI parity marker.

## Catching the local runner up later

When CI evolves, use this order:

1. Run `./tools/compat_surface/run_theos_preflight.sh status`.
2. If `CI workflow parity` reports `DRIFT`, compare
   `.github/workflows/theos-sdk-preflight.yml` with
   `local_theos_preflight.py` and `run_theos_preflight.sh`.
3. Port only substantive computation changes. GitHub-only reporting and PR
   comment logic should stay in the workflow.
4. Run the focused local tests/shards needed to prove the update.
5. Update `local_theos_preflight_baseline.json` with the workflow blob SHA,
   source head, and the Theos/fast-analyzer run numbers used as the new parity
   point.
6. Let normal GitHub CI independently validate the resulting commit.

The baseline file is intentionally a human-readable checkpoint, not an
automatic claim that any future CI run is equivalent.
