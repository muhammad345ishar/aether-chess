#!/usr/bin/env python3
"""Exercise benchmark deadlines and uncertainty reporting with python-chess."""

from __future__ import annotations

import sys
import time
import hashlib
import json
import shlex
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def fake_engine() -> None:
    """A real UCI child process that accepts searches but never finishes one."""
    for line in sys.stdin:
        command = line.strip()
        if command == "uci":
            print("id name Deadline regression engine\nuciok", flush=True)
        elif command == "isready":
            print("readyok", flush=True)
        elif command == "quit":
            return


if len(sys.argv) > 1 and sys.argv[1] == "--fake-engine":
    fake_engine()
    raise SystemExit(0)

import chess
import chess.engine

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from engine_runner import bounded_play
from experiment_report import engine_metadata, output_path
from run_matches import adjudicate, confidence_interval


def test_deadline(limit: chess.engine.Limit) -> None:
    # Register the executor first so engines close before its context waits for
    # workers. This is also the lifecycle required by benchmark callers.
    with ThreadPoolExecutor(max_workers=1) as executor:
        with chess.engine.SimpleEngine.popen_uci(
            [sys.executable, str(Path(__file__).resolve()), "--fake-engine"], timeout=2
        ) as engine:
            started = time.monotonic()
            try:
                bounded_play(engine, chess.Board(), limit, executor=executor, timeout=0.2)
            except TimeoutError:
                pass
            else:
                raise AssertionError("a nonresponding engine did not time out")
            elapsed = time.monotonic() - started
            assert elapsed >= 0.9 * (0.2 + (limit.time or 0)), "deadline omitted the thinking-time allowance"
            assert elapsed < 2, f"response deadline took {elapsed:.3f} seconds"
            # A deadline is not enough if an orphaned process or blocked worker
            # can still hang context-manager cleanup at the end of the run.
            engine.returncode.result(timeout=1)
            assert executor.submit(lambda: True).result(timeout=1)


def test_limit_preserved() -> None:
    board = chess.Board()
    limit = chess.engine.Limit(nodes=137)
    game = object()
    expected = chess.engine.PlayResult(chess.Move.from_uci("e2e4"), None)

    class ResponsiveEngine:
        def play(self, actual_board, actual_limit, **kwargs):
            assert actual_board is board
            assert actual_limit is limit and actual_limit.time is None
            assert kwargs == {"game": game, "info": chess.engine.INFO_ALL}
            return expected

        def close(self):
            raise AssertionError("a successful engine should stay open")

    with ThreadPoolExecutor(max_workers=1) as executor:
        reply = bounded_play(
            ResponsiveEngine(), board, limit, executor=executor, timeout=1,
            game=game, info=chess.engine.INFO_ALL,
        )
        assert reply is expected


def test_confidence_boundaries() -> None:
    assert confidence_interval([]) is None
    assert confidence_interval([0.5]) is None
    assert confidence_interval([1.0, 1.0]) is None
    assert confidence_interval([0.0, 0.0]) is None
    assert confidence_interval([0.5, 0.5]) is None
    scores = [0.0, 0.25, 0.5, 0.75, 1.0]
    interval = confidence_interval(scores, samples=1000, seed=17)
    assert interval is not None and 0 <= interval[0] < interval[1] <= 1
    assert interval[0] <= 0.5 <= interval[1]
    assert interval == confidence_interval(scores, samples=1000, seed=17)


def test_adjudication() -> None:
    mate = chess.Board("7k/5Q2/5K2/8/8/8/8/8 w - - 99 1")
    assert mate.can_claim_fifty_moves(), "fixture must offer a prospective draw claim"
    assert adjudicate(mate) is None, "a potential draw claim must not prevent mate in one"
    mate.push_uci("f7g7")
    outcome = adjudicate(mate)
    assert mate.halfmove_clock == 100
    assert outcome is not None and outcome.termination == chess.Termination.CHECKMATE
    assert outcome.winner == chess.WHITE

    draw = chess.Board("7k/8/5K2/8/8/8/8/Q7 w - - 100 1")
    outcome = adjudicate(draw)
    assert outcome is not None and outcome.termination == chess.Termination.FIFTY_MOVES
    assert outcome.winner is None

    repetition = chess.Board()
    cycle = ("g1f3", "g8f6", "f3g1", "f6g8")
    for move in cycle:
        repetition.push_uci(move)
    assert repetition.is_repetition(2) and not repetition.is_repetition(3)
    assert adjudicate(repetition) is None, "twofold repetition must not end the match"
    for move in cycle[:-1]:
        repetition.push_uci(move)
    assert repetition.can_claim_threefold_repetition() and not repetition.is_repetition(3)
    assert adjudicate(repetition) is None, "a prospective repetition must not end the match"
    repetition.push_uci(cycle[-1])
    outcome = adjudicate(repetition)
    assert outcome is not None and outcome.termination == chess.Termination.THREEFOLD_REPETITION
    assert outcome.winner is None


def reporting_engine(mode: str) -> None:
    """Small legal-move engine, optionally failing at a particular lifecycle stage."""
    board = chess.Board()
    searches = 0
    for line in sys.stdin:
        command = line.strip()
        if command == "uci":
            maximum = 1 if mode == "configuration" else 1024
            print(f"id name Artifact regression engine\noption name Hash type spin default 1 min 1 max {maximum}\nuciok", flush=True)
        elif command == "isready":
            print("readyok", flush=True)
        elif command.startswith("setoption") and mode == "configuration":
            return
        elif command.startswith("position "):
            position, _, moves = command[9:].partition(" moves ")
            board = chess.Board() if position == "startpos" else chess.Board(position[4:])
            for move in moves.split():
                board.push_uci(move)
        elif command.startswith("go "):
            searches += 1
            if mode == "midrun" and searches > 1:
                return
            move = next(iter(board.legal_moves), chess.Move.null())
            print(f"info depth 1 score cp 0 nodes 1 time 0 pv {move.uci()}\nbestmove {move.uci()}", flush=True)
        elif command == "quit":
            return


def test_report_preservation() -> None:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="aether-report-tests-") as directory:
        work = Path(directory)

        def executable(mode: str) -> Path:
            path = work / mode
            command = shlex.join([sys.executable, str(Path(__file__).resolve()), "--reporting-engine", mode])
            path.write_text(f"#!/bin/sh\nexec {command}\n")
            path.chmod(0o755)
            return path

        responsive = executable("responsive")
        midrun = executable("midrun")
        configuration = executable("configuration")
        missing = work / "missing-engine"
        suite = work / "suite.json"
        suite.write_text(json.dumps([{"name": name, "fen": chess.STARTING_FEN} for name in ("first", "second")]))
        openings = work / "openings.json"
        openings.write_text(json.dumps([{"name": "start", "moves": []}]))

        for tool in ("run_suite", "run_matches", "compare_engines"):
            for stage, candidate in (("startup", missing), ("configuration", configuration),
                                     ("search", midrun), ("completed", responsive)):
                output = work / f"{tool}-{stage}.json"
                command = [sys.executable, str(root / "scripts" / f"{tool}.py"),
                           "--output", str(output), "--timeout", "2"]
                if tool == "run_suite":
                    command += ["--engine", str(candidate), "--suite", str(suite), "--nodes", "1"]
                elif tool == "run_matches":
                    command += ["--candidate", str(candidate), "--baseline", str(responsive),
                                "--openings", str(openings), "--pairs", "1", "--max-plies", "2", "--nodes", "1"]
                else:
                    command += ["--aether", str(candidate), "--reference", str(responsive), "--nodes", "1"]
                result = subprocess.run(command, text=True, capture_output=True, timeout=12)
                assert result.returncode == (0 if stage == "completed" else 1), (command, result.stdout, result.stderr)
                report = json.loads(output.read_text())
                assert report["finished_at"]
                if stage == "completed":
                    assert report["status"] == "completed" and not report["failures"]
                    completed = report["games"] if tool == "run_matches" else report["results"]
                    assert len(completed) == (8 if tool == "compare_engines" else 2)
                else:
                    assert report["status"] == "failed"
                    assert report["failures"][-1]["stage"] == stage, report
                    assert report["failures"][-1]["error_type"] and report["failures"][-1]["message"]
                assert "revision" in report["source"] and report["environment"]["python"]
                if tool != "compare_engines":
                    fixture = suite if tool == "run_suite" else openings
                    assert report["fixtures"][0]["sha256"] == hashlib.sha256(fixture.read_bytes()).hexdigest()
                if stage == "search":
                    completed = report["games"] if tool == "run_matches" else report["results"]
                    assert completed, "completed work vanished after a later engine failure"
                    assert report["failures"][-1].get("fen") or report["failures"][-1].get("position_command")
                    if tool == "run_suite":
                        assert report["completed"] == 1 and report["total"] == 2
                    if tool == "run_matches":
                        assert len(completed) == 2 and completed[0]["termination"] == "ply cap"
                        assert report["summary"]["engine_failures"] == 1
                        with output.with_suffix(".pgn").open() as pgn:
                            games = [chess.pgn.read_game(pgn), chess.pgn.read_game(pgn)]
                        assert all(game is not None and not game.errors for game in games)

        first, second = output_path(work, "suite", None), output_path(work, "suite", None)
        assert first != second and first.parent.parent == work / "results"
        assert first.parent.is_dir() and second.parent.is_dir()

        manifest = work / "manifest.json"
        digest = hashlib.sha256(responsive.read_bytes()).hexdigest()
        manifest.write_text(json.dumps({"binary_sha256": digest, "source_revision": "fixture"}))
        assert engine_metadata(responsive)["manifest"]["binary_hash_matches"] is True
        manifest.write_text(json.dumps({"binary_sha256": "wrong"}))
        metadata = engine_metadata(responsive)["manifest"]
        assert metadata["binary_hash_matches"] is False and metadata["warning"]


def main() -> int:
    test_deadline(chess.engine.Limit(nodes=1))
    test_deadline(chess.engine.Limit(time=0.1))
    test_limit_preserved()
    test_confidence_boundaries()
    test_adjudication()
    test_report_preservation()
    print("tooling regressions ok")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--reporting-engine":
        reporting_engine(sys.argv[2])
        raise SystemExit(0)
    raise SystemExit(main())
