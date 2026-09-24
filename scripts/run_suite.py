#!/usr/bin/env python3
"""Run reproducible tactical/endgame cases with independent move validation."""

from __future__ import annotations

import argparse
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
from experiment_report import engine_metadata, output_path, record_failure, run_metadata, utc_now, write_report


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=root / "build/aether")
    parser.add_argument("--suite", type=Path, default=root / "tests/positions.json")
    parser.add_argument("--nodes", type=int, default=50000)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--output", type=Path, help="JSON path; default: a unique directory under results/")
    args = parser.parse_args()
    if args.nodes < 1 or args.timeout <= 0:
        parser.error("nodes and timeout must be positive")
    executable = args.engine.resolve()
    output = output_path(root, "suite", args.output)
    results = []
    metadata = engine_metadata(executable)
    report = {
        **run_metadata(root, (args.suite,)), "engine": str(executable), "sha256": metadata["sha256"],
        "engine_metadata": metadata, "suite": str(args.suite.resolve()), "node_limit": args.nodes,
        "options": {"Hash": 32}, "results": results, "passed": 0, "total": 0, "completed": 0,
    }
    write_report(output, report)
    print(f"Report: {output}", flush=True)
    stage, context = "fixtures", {}
    try:
        cases = json.loads(args.suite.read_text())
        if not isinstance(cases, list) or not cases:
            raise ValueError("suite must be a non-empty list")
        report["total"] = len(cases)
        prepared = []
        for case in cases:
            context = {"position": case["name"], "fen": case["fen"]}
            board = chess.Board(case["fen"])
            if not board.is_valid():
                raise ValueError(f"invalid suite FEN: {case['name']}")
            expected = set(case.get("bestmoves", []))
            for move in expected:
                if chess.Move.from_uci(move) not in board.legal_moves:
                    raise ValueError(f"illegal expected move {move}: {case['name']}")
            prepared.append((case, board, expected))
        stage, context = "startup", {"engine": str(executable)}
        with ThreadPoolExecutor(max_workers=1) as worker, chess.engine.SimpleEngine.popen_uci(str(executable), timeout=args.timeout) as engine:
            stage = "configuration"
            engine.configure({"Hash": 32})
            for index, (case, board, expected) in enumerate(prepared):
                stage, context = "search", {"engine": str(executable), "position": case["name"], "fen": case["fen"]}
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
                report.update(passed=sum(result["passed"] for result in results), completed=len(results))
                write_report(output, report)
                print(f"{'PASS' if passed else 'FAIL'} {case['name']}: {move}, {result['score']}, depth {result['depth']}", flush=True)
            stage, context = "shutdown", {"engine": str(executable)}
    except (Exception, KeyboardInterrupt) as exc:
        record_failure(report, stage, exc, **context)
        write_report(output, report)
        print(f"{stage} failed: {exc}; partial report saved to {output}", flush=True)
        return 1
    report.update(status="completed", finished_at=utc_now(), success=report["passed"] == report["total"])
    write_report(output, report)
    print(f"{report['passed']}/{report['total']} positions passed")
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
