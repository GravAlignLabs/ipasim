#!/usr/bin/env python3
"""Jev-assisted candidate triage for ipaSim compatibility work.

This tool is deliberately advisory. It sends a bounded, public evidence packet to
TypeSafe Jev and prints typed probabilities. It never edits source, runs a build,
or converts an ipaSim failure into success.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

DEFAULT_BASE_URL = "https://api.typesafe.ai"
DEFAULT_MODEL = "jev-latest"
SUPPORTED_TYPES = {"choice", "noul"}


class JevForgeError(RuntimeError):
    """Raised for an invalid input or failed Jev request."""


def load_input(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise JevForgeError(f"input file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise JevForgeError(
            f"invalid JSON in {path}: line {exc.lineno}, column {exc.colno}: {exc.msg}"
        ) from exc

    if not isinstance(payload, dict):
        raise JevForgeError("input root must be a JSON object")
    return payload


def validate_input(payload: dict[str, Any]) -> None:
    if "state" not in payload:
        raise JevForgeError("input must contain 'state'")

    questions = payload.get("questions")
    if not isinstance(questions, dict) or not questions:
        raise JevForgeError("input must contain a non-empty 'questions' object")

    for key, question in questions.items():
        if not isinstance(key, str) or not key:
            raise JevForgeError("question keys must be non-empty strings")
        if not isinstance(question, dict):
            raise JevForgeError(f"question '{key}' must be an object")

        qtype = question.get("type")
        if qtype not in SUPPORTED_TYPES:
            raise JevForgeError(
                f"question '{key}' has unsupported type {qtype!r}; "
                f"supported: {', '.join(sorted(SUPPORTED_TYPES))}"
            )

        instructions = question.get("instructions")
        if not isinstance(instructions, str) or not instructions.strip():
            raise JevForgeError(f"question '{key}' needs non-empty instructions")

        criteria = question.get("criteria")
        if qtype == "choice":
            if not isinstance(criteria, dict):
                raise JevForgeError(f"choice '{key}' needs a criteria object")
            if not 2 <= len(criteria) <= 255:
                raise JevForgeError(
                    f"choice '{key}' must have between 2 and 255 options"
                )
            for option in criteria:
                if not isinstance(option, str) or not option:
                    raise JevForgeError(
                        f"choice '{key}' option names must be non-empty strings"
                    )
        elif criteria is not None:
            if not isinstance(criteria, dict) or set(criteria) != {"true", "false"}:
                raise JevForgeError(
                    f"noul '{key}' criteria must contain exactly 'true' and 'false'"
                )

    model = payload.get("model")
    if model is not None and (not isinstance(model, str) or not model.strip()):
        raise JevForgeError("'model' must be a non-empty string when provided")


def build_request(payload: dict[str, Any], model_override: str | None) -> dict[str, Any]:
    model = (
        model_override
        or payload.get("model")
        or os.getenv("TYPESAFE_DEFAULT_MODEL")
        or DEFAULT_MODEL
    )
    return {
        "state": payload["state"],
        "model": model,
        "questions": payload["questions"],
    }


def call_jev(request_payload: dict[str, Any]) -> dict[str, Any]:
    api_key = os.getenv("TYPESAFE_API_KEY")
    if not api_key:
        raise JevForgeError(
            "TYPESAFE_API_KEY is not set. Set it in the environment or use --dry-run."
        )

    base_url = os.getenv("TYPESAFE_BASE_URL", DEFAULT_BASE_URL).rstrip("/")
    if not base_url.startswith("https://"):
        raise JevForgeError("TYPESAFE_BASE_URL must use HTTPS")

    url = f"{base_url}/v1/systemone"
    body = json.dumps(request_payload, separators=(",", ":")).encode("utf-8")
    request = Request(
        url,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "ipaSim-JevForge/0.1",
        },
    )

    try:
        with urlopen(request, timeout=20) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except HTTPError as exc:
        diagnostic = exc.read().decode("utf-8", errors="replace")
        raise JevForgeError(
            f"TypeSafe API returned HTTP {exc.code} {exc.reason}: {diagnostic}"
        ) from exc
    except URLError as exc:
        raise JevForgeError(f"TypeSafe API request failed: {exc.reason}") from exc

    try:
        result = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise JevForgeError(f"TypeSafe API returned invalid JSON: {raw}") from exc

    if not isinstance(result, dict):
        raise JevForgeError("TypeSafe API returned a non-object response")
    if not isinstance(result.get("answers"), dict):
        raise JevForgeError(
            "TypeSafe API response is missing an 'answers' object: "
            + json.dumps(result, ensure_ascii=False)
        )
    return result


def percent(value: Any) -> str:
    if isinstance(value, (int, float)):
        return f"{100.0 * float(value):.1f}%"
    return "n/a"


def print_human(result: dict[str, Any]) -> None:
    model = result.get("model", "unknown")
    print(f"Jev model: {model}")

    usage = result.get("usage")
    if isinstance(usage, dict):
        print(
            "Usage: "
            f"input_tokens={usage.get('input_tokens', 'n/a')} "
            f"output_tokens={usage.get('output_tokens', 'n/a')}"
        )
    print()

    answers = result["answers"]
    for key, answer in answers.items():
        print(key)
        if not isinstance(answer, dict):
            print(f"  unexpected answer: {answer!r}")
            continue

        answer_type = answer.get("type")
        if answer_type == "choice":
            print(f"  selected: {answer.get('choice', 'n/a')}")
            probabilities = answer.get("probabilities")
            if isinstance(probabilities, dict):
                ranked = sorted(
                    probabilities.items(),
                    key=lambda item: item[1] if isinstance(item[1], (int, float)) else -1,
                    reverse=True,
                )
                for option, probability in ranked:
                    print(f"  {option}: {percent(probability)}")
            if "confidence" in answer:
                print(f"  confidence: {percent(answer.get('confidence'))}")
        elif answer_type == "noul":
            print(f"  true: {percent(answer.get('noul'))}")
        else:
            print("  " + json.dumps(answer, ensure_ascii=False, sort_keys=True))
        print()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Rank bounded ipaSim compatibility candidates with TypeSafe Jev."
    )
    parser.add_argument("input", type=Path, help="JevForge JSON input file")
    parser.add_argument(
        "--model",
        help="Override the model in the input/TYPESAFE_DEFAULT_MODEL",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the complete TypeSafe response as JSON",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and print the outgoing request without network access",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])

    try:
        payload = load_input(args.input)
        validate_input(payload)
        request_payload = build_request(payload, args.model)

        if args.dry_run:
            print(json.dumps(request_payload, indent=2, ensure_ascii=False))
            return 0

        result = call_jev(request_payload)
        if args.json:
            print(json.dumps(result, indent=2, ensure_ascii=False, sort_keys=True))
        else:
            print_human(result)
        return 0
    except JevForgeError as exc:
        print(f"JevForge error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
