#!/usr/bin/env python3
"""Compare Aether perft with python-chess on targeted and seeded positions."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import sys
import time
from datetime import datetime, timezone
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
    if depth == 1:
        return board.legal_moves.count()
    total = 0
    for move in list(board.legal_moves):
        board.push(move)
        total += reference_perft(board, depth - 1)
        board.pop()
    return total


def identity(board: chess.Board) -> str:
    """Counters do not distinguish move trees; legal EP and castling rights do."""
    return " ".join(board.fen().split()[:4])


def seeded_cases(seed: int, count: int, stride: int, max_plies: int):
    rng = random.Random(seed)
    board = chess.Board()
    seen: set[str] = set()
    game = 0
    ply = 0
    attempts = 0
    while len(seen) < count:
        if board.is_game_over() or ply >= max_plies:
            board = chess.Board()
            game += 1
            ply = 0
        if ply % stride == 0 and identity(board) not in seen:
            seen.add(identity(board))
            yield {
                "name": f"seed {seed}, game {game}, ply {ply}",
                "kind": "random", "seed": seed, "game": game, "ply": ply,
                "moves": [move.uci() for move in board.move_stack],
                "fen": board.fen(en_passant="fen"),
            }
        board.push(rng.choice(list(board.legal_moves)))
        ply += 1
        attempts += 1
        if attempts > max(10000, count * stride * 100):
            raise RuntimeError(f"could not sample {count} distinct positions for seed {seed}")


def write_report(path: Path | None, report: dict) -> None:
    if path:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(report, indent=2) + "\n")
        temporary.replace(path)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("--seed", type=int, action="append", help="repeat for independent seeds (default: 20260913)")
    parser.add_argument("--count", type=int, default=40, help="distinct random positions per seed")
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--stride", type=int, default=4, help="plies between random samples")
    parser.add_argument("--max-plies", type=int, default=160, help="restart each random game after this many plies")
    parser.add_argument("--targeted", type=Path, default=root / "tests/perft_positions.json")
    parser.add_argument("--skip-targeted", action="store_true")
    parser.add_argument("--timeout", type=float, default=20, help="engine response deadline per position, seconds")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.count < 0 or args.depth < 0 or args.stride < 1 or args.max_plies <= args.stride or args.timeout <= 0:
        parser.error("count/depth must be nonnegative, stride/timeout positive, and max-plies larger than stride")
    seeds = args.seed or [20260913]
    if len(seeds) != len(set(seeds)):
        parser.error("seeds must be distinct")
    executable = args.engine.resolve()
    results: list[dict] = []
    report = {
        "schema_version": 2, "started_utc": datetime.now(timezone.utc).isoformat(),
        "engine": str(executable), "seeds": seeds, "count_per_seed": args.count,
        "depth": args.depth, "stride": args.stride, "max_plies": args.max_plies,
        "python_chess_version": chess.__version__, "results": results,
        "positions": 0, "passed": False, "status": "running",
    }
    write_report(args.output, report)
    try:
        report["sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        cases = [] if args.skip_targeted else [dict(case, kind="targeted") for case in json.loads(args.targeted.read_text())]
        for seed in seeds:
            cases.extend(seeded_cases(seed, args.count, args.stride, args.max_plies))
        report["planned_positions"] = len(cases)
        if not cases:
            raise ValueError("no positions selected")
        with UciEngine(executable, timeout=args.timeout) as engine:
            for case in cases:
                result = dict(case, depth=args.depth, passed=False)
                results.append(result)
                started = time.monotonic()
                try:
                    board = chess.Board(case["fen"])
                    if not board.is_valid():
                        raise ValueError("invalid fixture FEN")
                    for text in case.get("legal", []):
                        if chess.Move.from_uci(text) not in board.legal_moves:
                            raise ValueError(f"fixture requires illegal move {text}")
                    for text in case.get("illegal", []):
                        if chess.Move.from_uci(text) in board.legal_moves:
                            raise ValueError(f"fixture excludes legal move {text}")
                    expected = reference_perft(board, args.depth)
                    result["expected"] = expected
                    engine.send("position fen " + case["fen"])
                    engine.send(f"perft {args.depth}")
                    result["actual"] = int(engine.read_line())
                    result["passed"] = result["actual"] == expected
                    if not result["passed"]:
                        result["reference_legal_moves"] = sorted(move.uci() for move in board.legal_moves)
                        raise AssertionError(f"expected {expected}, got {result['actual']}")
                except Exception as exc:
                    result["error"] = f"{type(exc).__name__}: {exc}"
                    raise RuntimeError(f"{case['name']}: {case['fen']} at depth {args.depth}: {exc}") from exc
                finally:
                    result["wall_ms"] = round((time.monotonic() - started) * 1000, 3)
                    report["positions"] = sum(item["passed"] for item in results)
                    write_report(args.output, report)
        report["passed"] = True
        report["status"] = "complete"
    except Exception as exc:
        report["status"] = "failed"
        report["error"] = f"{type(exc).__name__}: {exc}"
        print(report["error"], file=sys.stderr)
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_report(args.output, report)
    print(f"Independent perft: {report['positions']}/{report.get('planned_positions', '?')} positions passed at depth {args.depth}; seeds={seeds}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
