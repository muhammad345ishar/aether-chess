#!/usr/bin/env python3
"""Run reproducible tactical/endgame cases with independent move validation."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    import chess
    import chess.engine
except ImportError as exc:
    raise SystemExit("Install benchmark dependencies: python -m pip install -r scripts/requirements.txt") from exc

from engine_runner import bounded_play


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=root / "build/aether")
    parser.add_argument("--suite", type=Path, default=root / "tests/positions.json")
    parser.add_argument("--nodes", type=int, default=50000)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.nodes < 1 or args.timeout <= 0:
        parser.error("nodes and timeout must be positive")
    executable = args.engine.resolve()
    cases = json.loads(args.suite.read_text())
    results = []
    with ThreadPoolExecutor(max_workers=1) as worker, chess.engine.SimpleEngine.popen_uci(str(executable), timeout=args.timeout) as engine:
        engine.configure({"Hash": 32})
        for index, case in enumerate(cases):
            board = chess.Board(case["fen"])
            if not board.is_valid():
                raise ValueError(f"invalid suite FEN: {case['name']}")
            expected = set(case.get("bestmoves", []))
            for move in expected:
                if chess.Move.from_uci(move) not in board.legal_moves:
                    raise ValueError(f"illegal expected move {move}: {case['name']}")
            started = time.monotonic()
            reply = bounded_play(engine, board, chess.engine.Limit(nodes=args.nodes), executor=worker,
                                 timeout=args.timeout, game=index, info=chess.engine.INFO_ALL)
            legal = reply.move in board.legal_moves if reply.move else board.is_game_over()
            move = reply.move.uci() if reply.move else "0000"
            passed = legal and (not expected or move in expected)
            if "mate" in case:
                score = reply.info.get("score")
                passed = passed and score is not None and score.pov(board.turn).mate() == case["mate"]
            if case.get("draw"):
                score = reply.info.get("score")
                passed = passed and score is not None and score.pov(board.turn).score() == 0
            result = {
                "name": case["name"], "fen": case["fen"], "passed": bool(passed), "bestmove": move,
                "expected": sorted(expected), "score": str(reply.info.get("score")),
                "depth": reply.info.get("depth", 0), "nodes": reply.info.get("nodes", 0),
                "wall_ms": round((time.monotonic() - started) * 1000, 3),
            }
            results.append(result)
            print(f"{'PASS' if passed else 'FAIL'} {case['name']}: {move}, {result['score']}, depth {result['depth']}", flush=True)
    report = {
        "engine": str(executable), "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "suite": str(args.suite.resolve()), "node_limit": args.nodes, "results": results,
        "passed": sum(result["passed"] for result in results), "total": len(results),
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['passed']}/{report['total']} positions passed")
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
