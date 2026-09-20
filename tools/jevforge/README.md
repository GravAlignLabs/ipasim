# JevForge

JevForge is an **advisory, local-only** experiment for ipaSim compatibility work.

It sends public, target-neutral compatibility evidence plus a bounded set of candidate approaches to TypeSafe Jev, then records Jev's typed probabilities so maintainers can decide which candidates are worth compiling and testing first.

JevForge does **not**:

- modify ipaSim runtime behavior on its own;
- decide whether a build, smoke test, or RuntimeRoot run passes;
- replace compiler/linker/runtime evidence;
- generate application-specific compatibility behavior;
- create success stubs, aliases, monkey patches, or hidden fallbacks;
- send RuntimeRoot contents, private application data, or binary contents to TypeSafe.

The intended loop is:

```text
real public compatibility evidence
        -> bounded candidate approaches
        -> Jev ranking / probabilities
        -> reviewed candidate patches
        -> isolated local worktrees
        -> real local compile + semantic smoke
        -> normal ipaSim validation
```

## Requirements

- Python 3.10+
- Git
- a TypeSafe API key from the TypeSafe console for live Jev ranking
- the normal local toolchain required by whatever compile/smoke commands an experiment manifest requests

No third-party Python package is required; the TypeSafe request and local runner use the Python standard library.

Set the key for the current PowerShell session:

```powershell
$env:TYPESAFE_API_KEY = "<your key>"
```

Or from Command Prompt:

```bat
set TYPESAFE_API_KEY=<your key>
```

Do not commit the key or put it in an input fixture.

## Run the public Jev pilot

```powershell
py tools/jevforge/jevforge.py tools/jevforge/examples/host_get_special_port.json
```

Machine-readable output:

```powershell
py tools/jevforge/jevforge.py tools/jevforge/examples/host_get_special_port.json --json
```

Validate and display the outgoing payload without contacting TypeSafe:

```powershell
py tools/jevforge/jevforge.py tools/jevforge/examples/host_get_special_port.json --dry-run
```

The pilot evaluates descriptions of candidate approaches. A candidate is only a suggestion for investigation until the real compiler, semantic smoke, and applicable ipaSim validation prove it.

## Local experiment runner

`runner.py` turns a bounded candidate set into isolated local experiments. It never edits the worktree from which you launch it.

For each selected candidate it:

1. resolves an explicit Git base ref to a commit SHA;
2. creates a temporary **detached Git worktree**;
3. runs `git apply --check` for every reviewed candidate patch;
4. applies the patch only inside that temporary worktree;
5. runs the configured compile/smoke commands in order;
6. captures the real stdout, stderr, exit code, timeout state, and duration;
7. prints the first actionable failing step directly;
8. removes the temporary worktree unless `--keep-worktrees` is requested.

A nonzero compiler, linker, smoke, or other command exit remains a failed candidate. Jev cannot override it.

The TypeSafe API key is removed from the environment passed to candidate build/smoke processes after ranking, so candidate code does not need or receive that credential.

### Smoke the runner itself

This smoke does **not** modify ipaSim runtime code. It only proves worktree isolation, command execution, diagnostic capture, and cleanup:

```powershell
py tools/jevforge/runner.py tools/jevforge/examples/runner_smoke.json --no-jev --output runner-smoke-result.json
```

To inspect the plan without creating a worktree:

```powershell
py tools/jevforge/runner.py tools/jevforge/examples/runner_smoke.json --no-jev --plan
```

### Ranked candidate experiments

A runner manifest may add:

```json
{
  "jev": {
    "input": "host_get_special_port.json",
    "ranking_question": "investigate_first",
    "max_candidates": 2,
    "min_probability": 0.05,
    "stop_if_none_selected": true
  }
}
```

Candidate ids in the runner manifest must match the option ids in the selected Jev `choice` question. If Jev selects `NONE`, the runner executes nothing by default.

Each candidate may reference one or more reviewed unified-diff patch files and candidate-specific commands. Common commands can also be declared once at manifest level.

Example shape:

```json
{
  "name": "one compatibility boundary",
  "base_ref": "origin/master",
  "commands": [
    {
      "name": "configure",
      "argv": ["cmake", "-S", ".", "-B", "build/jevforge"],
      "timeout_seconds": 600
    }
  ],
  "candidates": {
    "A": {
      "description": "reviewed candidate A",
      "patches": ["../candidates/boundary/A.patch"],
      "commands": [
        {
          "name": "build",
          "argv": ["cmake", "--build", "build/jevforge", "--config", "Release"],
          "timeout_seconds": 1200
        }
      ]
    }
  }
}
```

On Windows, batch files should be invoked explicitly through `cmd`, for example:

```json
{"argv": ["cmd", "/d", "/s", "/c", "some-script.cmd"]}
```

The runner uses `shell=False`; it does not implicitly reinterpret command strings.

Useful runner options:

- `--only ID` — run a specific candidate; repeat for more than one;
- `--no-jev` — skip live ranking and use manifest order or `--only`;
- `--plan` — resolve the base and candidate selection without creating worktrees;
- `--output FILE.json` — preserve the complete machine-readable experiment record;
- `--keep-worktrees` — keep temporary candidate worktrees for manual inspection.

## Jev input format

Each Jev JSON input contains:

- `state`: public, target-neutral evidence, constraints, and bounded candidate descriptions;
- `questions`: TypeSafe `choice` or `noul` questions;
- optional `model`: defaults to `TYPESAFE_DEFAULT_MODEL` or `jev-latest`.

The script validates the question shapes before any network request. Choice questions must include 2-255 criteria options. Noul criteria, when provided, must define both `true` and `false`.

## Runner safety boundary

The runner accepts local experiment manifests and patch files as executable engineering inputs. Review them before running them. Compile/smoke commands execute real local programs from isolated candidate worktrees.

The runner deliberately does not:

- generate source code;
- write candidate changes into the launch worktree;
- commit or push candidate changes;
- call GitHub Actions;
- convert failed commands into success;
- automatically promote a surviving candidate into ipaSim.

A candidate that survives the local runner is still only a **survivor**. Normal review and ipaSim validation remain required before integration.

## Environment

- `TYPESAFE_API_KEY` — required for a live Jev request.
- `TYPESAFE_BASE_URL` — optional; defaults to `https://api.typesafe.ai`.
- `TYPESAFE_DEFAULT_MODEL` — optional; defaults to `jev-latest`.
