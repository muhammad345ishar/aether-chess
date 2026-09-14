#!/usr/bin/env python3
"""Exercise benchmark deadlines and uncertainty reporting with python-chess."""

from __future__ import annotations

import sys
import time
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


def main() -> int:
    test_deadline(chess.engine.Limit(nodes=1))
    test_deadline(chess.engine.Limit(time=0.1))
    test_limit_preserved()
    test_confidence_boundaries()
    test_adjudication()
    print("tooling regressions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
