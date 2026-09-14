#!/usr/bin/env python3
"""Compare move-tree counts with python-chess on a seeded legal playout."""

from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

try:
    import chess
except ImportError as exc:
    raise SystemExit("Install dependencies: python -m pip install -r scripts/requirements.txt") from exc

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from uci_client import UciEngine


def reference_perft(board: chess.Board, depth: int) -> int:
    if depth == 0:
        return 1
    total = 0
    for move in list(board.legal_moves):
        board.push(move)
        total += reference_perft(board, depth - 1)
        board.pop()
    return total


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("--seed", type=int, default=20260913)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    board = chess.Board()
    checked = 0
    with UciEngine(args.engine) as engine:
        for ply in range(160):
            if board.is_game_over():
                board = chess.Board()
            if ply % 4 == 0:
                expected = reference_perft(board, 2)
                engine.send("position fen " + board.fen(en_passant="fen"))
                engine.send("perft 2")
                actual = int(engine.read_line())
                if actual != expected:
                    raise AssertionError(f"{board.fen()}: expected {expected}, got {actual}")
                checked += 1
            board.push(rng.choice(list(board.legal_moves)))
    print(f"Independent perft: {checked} positions passed at depth 2")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"seed": args.seed, "positions": checked, "depth": 2, "passed": True}, indent=2) + "\n")


if __name__ == "__main__":
    main()
