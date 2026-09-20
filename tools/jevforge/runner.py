#!/usr/bin/env python3
"""Run isolated local experiments for Jev-ranked ipaSim candidates.

The runner creates detached git worktrees from an explicit base ref, applies
reviewed patch files, and runs real local commands. Jev may rank candidates,
but toolchain exit codes are authoritative and are never converted to success.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import jevforge


class RunnerError(RuntimeError):
    """Raised for invalid runner configuration or repository state."""


SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]+$")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RunnerError(f"manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise RunnerError(
            f"invalid JSON in {path}: line {exc.lineno}, column {exc.colno}: {exc.msg}"
        ) from exc
    if not isinstance(payload, dict):
        raise RunnerError("manifest root must be a JSON object")
    return payload


def validate_command(command: Any, where: str) -> None:
    if not isinstance(command, dict):
        raise RunnerError(f"{where} must be an object")
    name = command.get("name")
    argv = command.get("argv")
    if not isinstance(name, str) or not name.strip():
        raise RunnerError(f"{where}.name must be a non-empty string")
    if not isinstance(argv, list) or not argv or not all(
        isinstance(item, str) and item for item in argv
    ):
        raise RunnerError(f"{where}.argv must be a non-empty array of strings")
    cwd = command.get("cwd", ".")
    if not isinstance(cwd, str) or not cwd:
        raise RunnerError(f"{where}.cwd must be a non-empty string")
    timeout = command.get("timeout_seconds", 600)
    if not isinstance(timeout, (int, float)) or timeout <= 0:
        raise RunnerError(f"{where}.timeout_seconds must be greater than zero")


def validate_manifest(manifest: dict[str, Any]) -> None:
    base_ref = manifest.get("base_ref", "HEAD")
    if not isinstance(base_ref, str) or not base_ref.strip():
        raise RunnerError("base_ref must be a non-empty string")

    candidates = manifest.get("candidates")
    if not isinstance(candidates, dict) or not candidates:
        raise RunnerError("manifest must contain a non-empty candidates object")

    for candidate_id, candidate in candidates.items():
        if not isinstance(candidate_id, str) or not SAFE_ID.fullmatch(candidate_id):
            raise RunnerError(
                f"candidate id {candidate_id!r} must match {SAFE_ID.pattern}"
            )
        if not isinstance(candidate, dict):
            raise RunnerError(f"candidate {candidate_id!r} must be an object")
        patches = candidate.get("patches", [])
        if not isinstance(patches, list) or not all(
            isinstance(item, str) and item for item in patches
        ):
            raise RunnerError(f"candidate {candidate_id}.patches must be a string array")
        commands = candidate.get("commands", [])
        if not isinstance(commands, list):
            raise RunnerError(f"candidate {candidate_id}.commands must be an array")
        for index, command in enumerate(commands):
            validate_command(command, f"candidate {candidate_id}.commands[{index}]")

    common_commands = manifest.get("commands", [])
    if not isinstance(common_commands, list):
        raise RunnerError("commands must be an array")
    for index, command in enumerate(common_commands):
        validate_command(command, f"commands[{index}]")

    jev = manifest.get("jev")
    if jev is not None:
        if not isinstance(jev, dict):
            raise RunnerError("jev must be an object")
        input_path = jev.get("input")
        ranking_question = jev.get("ranking_question")
        if not isinstance(input_path, str) or not input_path:
            raise RunnerError("jev.input must be a non-empty path string")
        if not isinstance(ranking_question, str) or not ranking_question:
            raise RunnerError("jev.ranking_question must be a non-empty string")
        max_candidates = jev.get("max_candidates", 1)
        if not isinstance(max_candidates, int) or max_candidates < 1:
            raise RunnerError("jev.max_candidates must be an integer >= 1")
        min_probability = jev.get("min_probability", 0.0)
        if not isinstance(min_probability, (int, float)) or not 0 <= min_probability <= 1:
            raise RunnerError("jev.min_probability must be between 0 and 1")


def run_process(
    argv: list[str],
    *,
    cwd: Path,
    timeout_seconds: float,
    env: dict[str, str] | None = None,
) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            argv,
            cwd=str(cwd),
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            timeout=timeout_seconds,
            shell=False,
        )
        return {
            "argv": argv,
            "exit_code": completed.returncode,
            "timed_out": False,
            "duration_seconds": round(time.monotonic() - started, 3),
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        return {
            "argv": argv,
            "exit_code": None,
            "timed_out": True,
            "duration_seconds": round(time.monotonic() - started, 3),
            "stdout": stdout,
            "stderr": stderr,
            "diagnostic": f"timed out after {timeout_seconds} seconds",
        }
    except OSError as exc:
        return {
            "argv": argv,
            "exit_code": None,
            "timed_out": False,
            "duration_seconds": round(time.monotonic() - started, 3),
            "stdout": "",
            "stderr": "",
            "diagnostic": f"could not start command: {exc}",
        }


def process_ok(result: dict[str, Any]) -> bool:
    return result.get("exit_code") == 0 and not result.get("timed_out", False)


def git_output(repo_root: Path, *args: str) -> str:
    result = run_process(
        ["git", *args], cwd=repo_root, timeout_seconds=60, env=os.environ.copy()
    )
    if not process_ok(result):
        diagnostic = result.get("stderr") or result.get("diagnostic") or result.get("stdout")
        raise RunnerError(f"git {' '.join(args)} failed: {diagnostic.strip()}")
    return result["stdout"].strip()


def discover_repo_root(start: Path) -> Path:
    return Path(git_output(start, "rev-parse", "--show-toplevel")).resolve()


def resolve_base(repo_root: Path, base_ref: str) -> str:
    return git_output(repo_root, "rev-parse", "--verify", f"{base_ref}^{{commit}}")


def resolve_manifest_path(manifest_path: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path.resolve()
    return (manifest_path.parent / path).resolve()


def format_token(value: str, *, worktree: Path, repo_root: Path, candidate_id: str) -> str:
    return (
        value.replace("{worktree}", str(worktree))
        .replace("{repo_root}", str(repo_root))
        .replace("{candidate_id}", candidate_id)
    )


def resolve_command_cwd(worktree: Path, cwd_value: str, *, repo_root: Path, candidate_id: str) -> Path:
    rendered = format_token(
        cwd_value, worktree=worktree, repo_root=repo_root, candidate_id=candidate_id
    )
    path = Path(rendered)
    if not path.is_absolute():
        path = worktree / path
    resolved = path.resolve()
    try:
        resolved.relative_to(worktree.resolve())
    except ValueError as exc:
        raise RunnerError(
            f"candidate {candidate_id} command cwd escapes its isolated worktree: {resolved}"
        ) from exc
    return resolved


def candidate_env() -> dict[str, str]:
    env = os.environ.copy()
    # Candidate code does not need the TypeSafe credential. Keep it out of build/smoke processes.
    env.pop("TYPESAFE_API_KEY", None)
    return env


def rank_with_jev(
    manifest: dict[str, Any], manifest_path: Path, *, model_override: str | None
) -> tuple[list[str], dict[str, Any]]:
    config = manifest["jev"]
    jev_path = resolve_manifest_path(manifest_path, config["input"])
    payload = jevforge.load_input(jev_path)
    jevforge.validate_input(payload)
    request_payload = jevforge.build_request(payload, model_override)
    result = jevforge.call_jev(request_payload)

    ranking_question = config["ranking_question"]
    answer = result.get("answers", {}).get(ranking_question)
    if not isinstance(answer, dict) or answer.get("type") != "choice":
        raise RunnerError(
            f"Jev answer {ranking_question!r} is missing or is not a choice answer"
        )
    probabilities = answer.get("probabilities")
    if not isinstance(probabilities, dict):
        raise RunnerError(f"Jev answer {ranking_question!r} has no probabilities object")

    candidates = manifest["candidates"]
    scores: dict[str, float] = {}
    for candidate_id in candidates:
        value = probabilities.get(candidate_id)
        if isinstance(value, (int, float)):
            scores[candidate_id] = float(value)

    if not scores:
        raise RunnerError(
            f"Jev answer {ranking_question!r} contains no probabilities for manifest candidates"
        )

    none_score = probabilities.get("NONE")
    selected_choice = answer.get("choice")
    if config.get("stop_if_none_selected", True) and selected_choice == "NONE":
        return [], {
            "source": "jev",
            "model": result.get("model"),
            "usage": result.get("usage"),
            "question": ranking_question,
            "selected_choice": selected_choice,
            "none_probability": none_score,
            "probabilities": probabilities,
            "reason": "Jev selected NONE; no candidate executed",
        }

    min_probability = float(config.get("min_probability", 0.0))
    max_candidates = int(config.get("max_candidates", 1))
    ordered = [
        candidate_id
        for candidate_id, score in sorted(scores.items(), key=lambda item: item[1], reverse=True)
        if score >= min_probability
    ][:max_candidates]

    return ordered, {
        "source": "jev",
        "model": result.get("model"),
        "usage": result.get("usage"),
        "question": ranking_question,
        "selected_choice": selected_choice,
        "none_probability": none_score,
        "probabilities": probabilities,
        "selected_candidates": ordered,
    }


def select_candidates(
    manifest: dict[str, Any], manifest_path: Path, args: argparse.Namespace
) -> tuple[list[str], dict[str, Any]]:
    candidates = list(manifest["candidates"].keys())

    if args.only:
        unknown = [candidate for candidate in args.only if candidate not in manifest["candidates"]]
        if unknown:
            raise RunnerError(f"unknown candidate(s) in --only: {', '.join(unknown)}")
        return args.only, {"source": "explicit", "selected_candidates": args.only}

    if manifest.get("jev") is not None and not args.no_jev:
        return rank_with_jev(manifest, manifest_path, model_override=args.model)

    limit = args.max_candidates if args.max_candidates is not None else len(candidates)
    selected = candidates[:limit]
    return selected, {"source": "manifest-order", "selected_candidates": selected}


def apply_patch(
    repo_root: Path,
    worktree: Path,
    patch_path: Path,
    candidate_id: str,
) -> list[dict[str, Any]]:
    if not patch_path.is_file():
        return [
            {
                "stage": "patch_missing",
                "patch": str(patch_path),
                "exit_code": None,
                "diagnostic": f"patch file not found: {patch_path}",
            }
        ]

    results: list[dict[str, Any]] = []
    for stage, extra in (
        ("patch_check", ["apply", "--check", str(patch_path)]),
        ("patch_apply", ["apply", "--whitespace=nowarn", str(patch_path)]),
    ):
        result = run_process(
            ["git", *extra],
            cwd=worktree,
            timeout_seconds=120,
            env=candidate_env(),
        )
        result.update({"stage": stage, "patch": str(patch_path)})
        results.append(result)
        if not process_ok(result):
            break
    return results


def run_candidate(
    *,
    repo_root: Path,
    base_sha: str,
    worktree_root: Path,
    manifest: dict[str, Any],
    manifest_path: Path,
    candidate_id: str,
    keep_worktrees: bool,
) -> dict[str, Any]:
    candidate = manifest["candidates"][candidate_id]
    worktree = (worktree_root / candidate_id).resolve()
    record: dict[str, Any] = {
        "id": candidate_id,
        "description": candidate.get("description", ""),
        "started_utc": utc_now(),
        "worktree": str(worktree),
        "status": "failed",
        "patches": [],
        "steps": [],
    }

    add_result = run_process(
        ["git", "worktree", "add", "--detach", str(worktree), base_sha],
        cwd=repo_root,
        timeout_seconds=120,
        env=os.environ.copy(),
    )
    record["worktree_add"] = add_result
    if not process_ok(add_result):
        record["status"] = "setup_failed"
        record["finished_utc"] = utc_now()
        return record

    try:
        for patch_value in candidate.get("patches", []):
            patch_path = resolve_manifest_path(manifest_path, patch_value)
            patch_results = apply_patch(repo_root, worktree, patch_path, candidate_id)
            record["patches"].extend(patch_results)
            if not all(process_ok(item) for item in patch_results):
                record["status"] = "patch_failed"
                record["finished_utc"] = utc_now()
                return record

        commands = [*manifest.get("commands", []), *candidate.get("commands", [])]
        for command in commands:
            cwd = resolve_command_cwd(
                worktree,
                command.get("cwd", "."),
                repo_root=repo_root,
                candidate_id=candidate_id,
            )
            argv = [
                format_token(
                    item,
                    worktree=worktree,
                    repo_root=repo_root,
                    candidate_id=candidate_id,
                )
                for item in command["argv"]
            ]
            result = run_process(
                argv,
                cwd=cwd,
                timeout_seconds=float(command.get("timeout_seconds", 600)),
                env=candidate_env(),
            )
            result.update({"name": command["name"], "cwd": str(cwd)})
            record["steps"].append(result)

            if not process_ok(result):
                record["status"] = "failed"
                record["finished_utc"] = utc_now()
                return record

        record["status"] = "passed"
        record["finished_utc"] = utc_now()
        return record
    finally:
        if keep_worktrees:
            record["worktree_kept"] = True
        else:
            remove_result = run_process(
                ["git", "worktree", "remove", "--force", str(worktree)],
                cwd=repo_root,
                timeout_seconds=120,
                env=os.environ.copy(),
            )
            record["worktree_remove"] = remove_result
            if worktree.exists():
                shutil.rmtree(worktree, ignore_errors=True)


def print_failure(candidate: dict[str, Any]) -> None:
    print(f"\n[{candidate['id']}] FAILED: {candidate['status']}")
    for patch in candidate.get("patches", []):
        if not process_ok(patch):
            print(f"stage: {patch.get('stage')}")
            print((patch.get("stderr") or patch.get("diagnostic") or patch.get("stdout") or "").rstrip())
            return
    for step in candidate.get("steps", []):
        if not process_ok(step):
            print(f"step: {step.get('name')}")
            print("command: " + " ".join(step.get("argv", [])))
            diagnostic = step.get("stderr") or step.get("diagnostic") or step.get("stdout") or ""
            print(diagnostic.rstrip())
            return
    add_result = candidate.get("worktree_add")
    if isinstance(add_result, dict) and not process_ok(add_result):
        print((add_result.get("stderr") or add_result.get("diagnostic") or "").rstrip())


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run isolated local toolchain experiments for Jev-ranked candidates."
    )
    parser.add_argument("manifest", type=Path, help="Runner experiment JSON manifest")
    parser.add_argument(
        "--only",
        action="append",
        help="Run only this candidate id; repeat to run multiple candidates",
    )
    parser.add_argument(
        "--no-jev",
        action="store_true",
        help="Skip Jev ranking and use manifest order (or --only)",
    )
    parser.add_argument("--model", help="Override the Jev model for ranking")
    parser.add_argument(
        "--max-candidates",
        type=int,
        help="Limit candidates when Jev ranking is disabled",
    )
    parser.add_argument(
        "--keep-worktrees",
        action="store_true",
        help="Keep candidate worktrees for manual inspection",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write the full machine-readable experiment result to this JSON file",
    )
    parser.add_argument(
        "--plan",
        action="store_true",
        help="Resolve selection/base ref and print the plan without creating worktrees",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    manifest_path = args.manifest.resolve()

    try:
        manifest = load_manifest(manifest_path)
        validate_manifest(manifest)
        repo_root = discover_repo_root(Path.cwd())
        base_ref = manifest.get("base_ref", "HEAD")
        base_sha = resolve_base(repo_root, base_ref)
        selected, selection = select_candidates(manifest, manifest_path, args)

        plan = {
            "experiment": manifest.get("name", manifest_path.stem),
            "repo_root": str(repo_root),
            "base_ref": base_ref,
            "base_sha": base_sha,
            "selection": selection,
        }
        if args.plan:
            print(json.dumps(plan, indent=2, ensure_ascii=False))
            return 0

        if not selected:
            print("JevForge runner: no candidate selected; nothing executed.")
            if args.output:
                args.output.write_text(
                    json.dumps({**plan, "candidates": []}, indent=2, ensure_ascii=False),
                    encoding="utf-8",
                )
            return 3

        result: dict[str, Any] = {
            **plan,
            "started_utc": utc_now(),
            "candidates": [],
        }

        worktree_root = Path(tempfile.mkdtemp(prefix="ipasim-jevforge-"))
        result["worktree_root"] = str(worktree_root)
        try:
            for candidate_id in selected:
                score = selection.get("probabilities", {}).get(candidate_id)
                if isinstance(score, (int, float)):
                    print(f"[{candidate_id}] Jev ranking: {100.0 * float(score):.1f}%")
                else:
                    print(f"[{candidate_id}] starting")

                candidate_result = run_candidate(
                    repo_root=repo_root,
                    base_sha=base_sha,
                    worktree_root=worktree_root,
                    manifest=manifest,
                    manifest_path=manifest_path,
                    candidate_id=candidate_id,
                    keep_worktrees=args.keep_worktrees,
                )
                result["candidates"].append(candidate_result)
                if candidate_result["status"] == "passed":
                    print(f"[{candidate_id}] PASS")
                else:
                    print_failure(candidate_result)
        finally:
            if not args.keep_worktrees:
                shutil.rmtree(worktree_root, ignore_errors=True)

        result["finished_utc"] = utc_now()
        result["passed"] = all(
            candidate.get("status") == "passed" for candidate in result["candidates"]
        )

        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8"
            )
            print(f"\nResult JSON: {args.output}")

        return 0 if result["passed"] else 1
    except (RunnerError, jevforge.JevForgeError) as exc:
        print(f"JevForge runner error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
