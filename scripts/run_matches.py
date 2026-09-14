#!/usr/bin/env python3
"""Paired games with independent adjudication, PGN output and uncertainty estimates."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import statistics
import time
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    import chess
    import chess.engine
    import chess.pgn
except ImportError as exc:
    raise SystemExit("Install benchmark dependencies: python -m pip install -r scripts/requirements.txt") from exc

from engine_runner import bounded_play


class MoveOverrun(TimeoutError):
    pass


def confidence_interval(pair_scores: list[float], samples: int = 20000, seed: int = 0) -> list[float] | None:
    """Bootstrap entire opening pairs: swapping colors makes games correlated.

    Degenerate/small samples cannot give a meaningful empirical interval and
    return None instead of a misleading zero-width interval.
    """
    if len(pair_scores) < 2 or len(set(pair_scores)) < 2:
        return None
    rng = random.Random(seed)
    means = sorted(statistics.fmean(rng.choices(pair_scores, k=len(pair_scores))) for _ in range(samples))
    return [means[int(samples * 0.025)], means[min(samples - 1, int(samples * 0.975))]]


def elo(score: float) -> float | None:
    return 400 * math.log10(score / (1 - score)) if 0 < score < 1 else None


def adjudicate(board: chess.Board) -> chess.Outcome | None:
    # Preserve mate precedence. Do not assume a player will choose a move that
    # permits a draw claim when a different move could win immediately.
    outcome = board.outcome(claim_draw=False)
    if outcome:
        return outcome
    if board.is_fifty_moves():
        return chess.Outcome(chess.Termination.FIFTY_MOVES, None)
    if board.is_repetition(3):
        return chess.Outcome(chess.Termination.THREEFOLD_REPETITION, None)
    return None


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, default=root / "build/aether")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--openings", type=Path, default=root / "tests/openings.json")
    parser.add_argument("--pairs", type=int, default=12)
    limits = parser.add_mutually_exclusive_group()
    limits.add_argument("--movetime", type=int, help="milliseconds per move; default 50")
    limits.add_argument("--nodes", type=int, help="fixed nodes per move for reproducible search comparisons")
    parser.add_argument("--max-plies", type=int, default=300)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--hash", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--overhead-ms", type=int, default=20, help="allowed per-move process/IPC overhead before a time forfeit")
    parser.add_argument("--output", type=Path, default=root / "build/match-results.json")
    parser.add_argument("--pgn", type=Path, default=root / "build/match-games.pgn")
    args = parser.parse_args()
    budget = args.nodes if args.nodes is not None else (50 if args.movetime is None else args.movetime)
    if min(args.pairs, args.max_plies, budget) <= 0 or not 1 <= args.hash <= 1024 or args.timeout <= 0 or args.overhead_ms < 0:
        parser.error("pairs, move budget, max plies and timeout must be positive; Hash must be 1..1024")
    book = json.loads(args.openings.read_text())
    if not book:
        parser.error("opening book is empty")
    if args.pairs > len(book):
        parser.error(f"only {len(book)} distinct opening pairs available; provide a larger book")
    random.Random(args.seed).shuffle(book)
    book = book[:args.pairs]
    for opening in book:
        board = chess.Board(opening.get("fen", chess.STARTING_FEN))
        if not board.is_valid():
            raise ValueError(f"invalid opening: {opening['name']}")
        for text in opening.get("moves", []):
            board.push_uci(text)
        if adjudicate(board):
            raise ValueError(f"opening is already over: {opening['name']}")
    paths = [args.candidate.resolve(), args.baseline.resolve()]
    report = {
        "engines": [{"role": role, "path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                    for role, path in zip(("candidate", "baseline"), paths)],
        "options": {"Hash": args.hash, "Threads": 1},
        "limit": {"nodes": args.nodes} if args.nodes is not None else {"movetime_ms": budget},
        "seed": args.seed, "max_plies": args.max_plies, "games": [],
        "timing_policy": {"wall_time_includes_position_and_ipc": True, "overhead_ms": args.overhead_ms,
                          "timed_move_forfeit_after_ms": budget + args.overhead_ms if args.nodes is None else None},
        "note": "Approximate 95% percentile bootstrap resamples opening pairs. Small samples do not establish a strength gain.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.pgn.parent.mkdir(parents=True, exist_ok=True)
    pair_scores = []
    all_scores = []
    failures = 0

    def save() -> None:
        interval = confidence_interval(pair_scores, seed=args.seed) if len(all_scores) == 2 * len(pair_scores) else None
        score = statistics.fmean(all_scores) if all_scores else 0.5
        report["summary"] = {
            "wins": all_scores.count(1.0), "draws": all_scores.count(0.5), "losses": all_scores.count(0.0),
            "score": score, "completed_pairs": len(pair_scores), "score_interval_95": interval,
            "elo_estimate": elo(score), "elo_interval_95": [elo(value) for value in interval] if interval else None,
            "engine_failures": failures,
            "adjudicated_at_ply_cap": sum(g["termination"] == "ply cap" for g in report["games"]),
        }
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    with ExitStack() as stack:
        worker = stack.enter_context(ThreadPoolExecutor(max_workers=1))
        engines = [stack.enter_context(chess.engine.SimpleEngine.popen_uci(str(path), timeout=args.timeout)) for path in paths]
        for engine in engines:
            engine.configure({name: value for name, value in (("Hash", args.hash), ("Threads", 1)) if name in engine.options})
        pgn_file = stack.enter_context(args.pgn.open("w"))
        for pair, opening in enumerate(book):
            current_scores = []
            for reverse in range(2):
                candidate_color = chess.WHITE if reverse == 0 else chess.BLACK
                board = chess.Board(opening.get("fen", chess.STARTING_FEN))
                game = chess.pgn.Game()
                game.setup(board)
                game.headers.update({
                    "Event": "Aether paired regression match", "Round": f"{pair + 1}.{reverse + 1}",
                    "White": "Aether candidate" if candidate_color == chess.WHITE else "Baseline",
                    "Black": "Baseline" if candidate_color == chess.WHITE else "Aether candidate",
                    "Opening": opening["name"],
                })
                node = game
                for text in opening.get("moves", []):
                    move = chess.Move.from_uci(text)
                    board.push(move)
                    node = node.add_variation(move)
                # python-chess suppresses unchanged options, so force a size
                # change when the engine has no Clear Hash button.
                for engine in engines:
                    if "Clear Hash" in engine.options:
                        engine.configure({"Clear Hash": None})
                    elif "Hash" in engine.options:
                        engine.configure({"Hash": 1 if args.hash != 1 else 2})
                        engine.configure({"Hash": args.hash})
                result = "1/2-1/2"
                termination = "ply cap"
                error = None
                timing = {"candidate": {"moves": 0, "total_ms": 0.0, "max_ms": 0.0},
                          "baseline": {"moves": 0, "total_ms": 0.0, "max_ms": 0.0}}
                game_token = object()
                for _ in range(args.max_plies):
                    outcome = adjudicate(board)
                    if outcome:
                        result = outcome.result()
                        termination = outcome.termination.name.lower()
                        break
                    index = 0 if board.turn == candidate_color else 1
                    limit = chess.engine.Limit(nodes=args.nodes) if args.nodes is not None else chess.engine.Limit(time=budget / 1000)
                    try:
                        started = time.monotonic()
                        reply = bounded_play(engines[index], board, limit, executor=worker,
                                             timeout=args.timeout, game=game_token)
                        elapsed = (time.monotonic() - started) * 1000
                        measured = timing["candidate" if index == 0 else "baseline"]
                        measured["moves"] += 1
                        measured["total_ms"] += elapsed
                        measured["max_ms"] = max(measured["max_ms"], elapsed)
                        if args.nodes is None and elapsed > budget + args.overhead_ms:
                            raise MoveOverrun(f"move took {elapsed:.1f} ms; allowed {budget + args.overhead_ms} ms")
                        if reply.move is None or reply.move not in board.legal_moves:
                            raise ValueError(f"illegal bestmove {reply.move}")
                    except (chess.engine.EngineError, TimeoutError, ValueError) as exc:
                        result = "0-1" if board.turn == chess.WHITE else "1-0"
                        termination = "time forfeit" if isinstance(exc, MoveOverrun) else "engine failure"
                        error = str(exc)
                        failures += 1
                        break
                    board.push(reply.move)
                    node = node.add_variation(reply.move)
                else:
                    outcome = adjudicate(board)
                    if outcome:
                        result = outcome.result()
                        termination = outcome.termination.name.lower()
                white_score = 1.0 if result == "1-0" else 0.0 if result == "0-1" else 0.5
                score = white_score if candidate_color == chess.WHITE else 1 - white_score
                current_scores.append(score)
                all_scores.append(score)
                game.headers["Result"] = result
                game.headers["Termination"] = termination
                print(game, file=pgn_file, end="\n\n")
                pgn_file.flush()
                record = {"pair": pair + 1, "opening": opening["name"], "candidate_color": chess.COLOR_NAMES[candidate_color],
                          "result": result, "candidate_score": score, "termination": termination, "plies": board.ply(),
                          "timing": timing}
                if error:
                    record["error"] = error
                report["games"].append(record)
                print(f"{pair + 1}.{reverse + 1} {opening['name']}: candidate {record['candidate_color']} {result} ({termination})", flush=True)
                if reverse:
                    pair_scores.append(statistics.fmean(current_scores))
                save()
                if error:
                    # A terminated engine cannot play further games. Preserve
                    # the partial report and make the failed run visible to CI.
                    print("Stopped after engine failure; partial JSON and PGN saved.", flush=True)
                    return 1
    print(json.dumps(report["summary"], indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
