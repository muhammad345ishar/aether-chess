#!/usr/bin/env python3
"""Validate every fixture in tests/perft_positions.json up front.

differential_perft.py gates each fixture on chess.Board(fen).is_valid() plus
its declared legal/illegal move-list assertions, but it fails fast on the first
bad fixture and only runs after the engine is built. This check applies the same
gate to every fixture in a single pass and reports all problems at once, so a
malformed FEN is caught early -- before the build -- instead of surfacing one at
a time behind a full perft comparison.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import chess
except ImportError as exc:
    raise SystemExit("Install dependencies: python -m pip install -r scripts/requirements.txt") from exc


def validate(cases: list[dict]) -> list[str]:
    """Return a human-readable problem for every fixture that would fail the gate."""
    problems: list[str] = []
    for index, case in enumerate(cases):
        label = case.get("name", f"index {index}")
        fen = case.get("fen")
        if not isinstance(fen, str):
            problems.append(f"{label}: missing or non-string 'fen'")
            continue
        try:
            board = chess.Board(fen)
        except ValueError as exc:
            problems.append(f"{label}: unparseable FEN {fen!r}: {exc}")
            continue
        if not board.is_valid():
            problems.append(f"{label}: illegal position ({board.status()!r}): {fen}")
        legal_moves = set(board.legal_moves)
        for text in case.get("legal", []):
            try:
                move = chess.Move.from_uci(text)
            except (ValueError, chess.InvalidMoveError) as exc:
                problems.append(f"{label}: bad uci in 'legal' {text!r}: {exc}")
                continue
            if move not in legal_moves:
                problems.append(f"{label}: 'legal' move {text} is rejected by python-chess: {fen}")
        for text in case.get("illegal", []):
            try:
                move = chess.Move.from_uci(text)
            except (ValueError, chess.InvalidMoveError) as exc:
                problems.append(f"{label}: bad uci in 'illegal' {text!r}: {exc}")
                continue
            if move in legal_moves:
                problems.append(f"{label}: 'illegal' move {text} is accepted by python-chess: {fen}")
    return problems


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, default=root / "tests/perft_positions.json")
    args = parser.parse_args()

    cases = json.loads(args.fixtures.read_text())
    if not isinstance(cases, list) or not cases:
        print(f"{args.fixtures}: expected a non-empty JSON array of fixtures", file=sys.stderr)
        return 1

    problems = validate(cases)
    for problem in problems:
        print(f"[invalid fixture] {problem}", file=sys.stderr)
    if problems:
        print(f"\n{len(problems)} problem(s) across {len(cases)} fixtures in {args.fixtures}", file=sys.stderr)
        return 1
    print(f"All {len(cases)} fixtures valid (is_valid + legal/illegal assertions) in {args.fixtures}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
