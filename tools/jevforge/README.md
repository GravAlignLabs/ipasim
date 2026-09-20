# JevForge

JevForge is an **advisory, local-only** experiment for ipaSim compatibility work.

It sends public, target-neutral compatibility evidence plus a bounded set of candidate approaches to TypeSafe Jev, then records Jev's typed probabilities so maintainers can decide which candidates are worth compiling and testing first.

JevForge does **not**:

- modify ipaSim runtime behavior;
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
        -> human or coding-agent review
        -> real local compile + semantic smoke
        -> normal ipaSim validation
```

## Requirements

- Python 3.10+
- a TypeSafe API key from the TypeSafe console

No third-party Python package is required by this prototype; it calls the documented TypeSafe System One HTTP endpoint directly.

Set the key for the current PowerShell session:

```powershell
$env:TYPESAFE_API_KEY = "<your key>"
```

Do not commit the key or put it in an input fixture.

## Run the public pilot

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

The pilot deliberately evaluates descriptions of candidate approaches, not source-code mutations. A candidate is only a suggestion for investigation until the real compiler, semantic smoke, and applicable ipaSim validation prove it.

## Input format

Each JSON input contains:

- `state`: public, target-neutral evidence, constraints, and bounded candidate descriptions;
- `questions`: TypeSafe `choice` or `noul` questions;
- optional `model`: defaults to `TYPESAFE_DEFAULT_MODEL` or `jev-latest`.

The script validates the question shapes before any network request. Choice questions must include 2-255 criteria options. Noul criteria, when provided, must define both `true` and `false`.

## Environment

- `TYPESAFE_API_KEY` — required for a live request.
- `TYPESAFE_BASE_URL` — optional; defaults to `https://api.typesafe.ai`.
- `TYPESAFE_DEFAULT_MODEL` — optional; defaults to `jev-latest`.

## Safety boundary

The first prototype intentionally stops before source generation or compilation. If this pilot proves useful, a later independently reviewed increment can add a local candidate executor. That executor must preserve the same rule: **Jev may prioritize experiments, but only real toolchain/runtime evidence can validate them.**
