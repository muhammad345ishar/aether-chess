#!/usr/bin/env python3
"""Exercise the actual threaded executable, including command ordering."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from uci_client import UciEngine


def check(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def main() -> None:
    executable = Path(sys.argv[1]).resolve()
    # Deliberately buffer stop before the worker can start: this was a reliable
    # lost-cancellation reproduction in Aether 0.1.
    commands = ["uci", "position startpos"]
    for _ in range(20):
        commands += ["go infinite", "stop"]
    commands += ["quit"]
    result = subprocess.run(
        [str(executable)], input="\n".join(commands) + "\n",
        text=True, capture_output=True, timeout=8,
    )
    check(result.returncode == 0, result.stderr)
    check(result.stdout.count("bestmove ") == 20, "each stopped search must produce one bestmove")

    result = subprocess.run(
        [str(executable)], input="position startpos\ngo infinite\n",
        text=True, capture_output=True, timeout=4,
    )
    check(result.returncode == 0 and result.stdout.count("bestmove ") == 1, "EOF must stop and join the worker")

    with UciEngine(executable, timeout=5) as engine:
        def perft() -> int:
            engine.send("perft 2")
            return int(engine.read_line())

        check(perft() == 400, "initial position perft")
        malformed = (
            "position fen 7k/8/8/8/8/8/8/4pK2 b - - 0 1",
            "position fen 7k/8/1N6/Pp6/8/8/8/K7 w - b6 0 1",
            "position startpos moves e2e4 e7e5 not-a-move",
            "position fen invalid",
            "position startpos unexpected",
        )
        for command in malformed:
            engine.send(command)
            check(engine.read_line().startswith("info string invalid position"), command)
            check(perft() == 400, f"rejected position must be atomic: {command}")

        for command in ("go depth -1", "go nodes -1", "go depth 4junk", "go movetime", "perft"):
            engine.send(command)
            check(engine.read_line().startswith("info string invalid"), command)

        engine.send("go nodes 1")
        lines = engine.read_until("bestmove")
        counts = [int(m.group(1)) for line in lines if (m := re.search(r"\bnodes (\d+)", line))]
        check(counts and max(counts) <= 1, f"node limit overshoot: {lines}")
        check(lines[-1].split()[1] != "0000", "short search still returns a legal fallback")

        for limit in ("depth 3", "movetime 20", "wtime 0 btime 0", "depth 2"):
            engine.send(f"go {limit}")
            lines = engine.read_until("bestmove", timeout=4)
            check(lines[-1].split()[1] != "0000", f"reusable search after stop: {limit}")

        # A knight returning to a position seen once before the search root is
        # not a threefold claim. Reuse one engine so TT entries survive reroots.
        queen_missing = "rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
        cycle = "g1f3 g8f6 f3g1 f6g8"

        def repetition_score(moves: str) -> tuple[int, str]:
            engine.send(f"position fen {queen_missing} moves {moves}")
            engine.send("go depth 4")
            lines = engine.read_until("bestmove", timeout=5)
            scores = [int(m.group(1)) for line in lines if (m := re.search(r"\bscore cp (-?\d+)", line))]
            check(bool(scores), f"missing repetition score: {lines}")
            return scores[-1], lines[-1].split()[1]

        engine.send("ucinewgame")
        score, best = repetition_score("g1f3 g8f6 f3g1")
        check(score < -500 and best != "0000", "a historical second occurrence hid the missing queen")
        score, _ = repetition_score(cycle)
        check(score > 500, "a second occurrence at the root was treated as threefold")
        score, best = repetition_score(f"{cycle} g1f3 g8f6 f3g1")
        check(score == 0 and best == "f6g8", "engine did not choose a genuine third occurrence")
        score, _ = repetition_score(f"{cycle} {cycle}")
        check(score == 0, "threefold at the root was not a draw")

        engine.send("position fen 7k/5Q2/5K2/8/8/8/8/8 w - - 99 1")
        engine.send("go infinite")
        # Mate should be reported promptly, but bestmove must wait for stop.
        lines = engine.read_until("info depth")
        check("score mate 1" in lines[-1], f"mate at fifty-move boundary: {lines}")
        engine.send("isready")
        lines = engine.read_until("readyok")
        check(not any(line.startswith("bestmove") for line in lines), "infinite search returned early")
        try:
            line = engine.read_line(timeout=0.1)
            check(not line.startswith("bestmove"), "infinite search returned before stop")
        except TimeoutError:
            pass
        engine.send("stop")
        check(engine.read_until("bestmove")[-1].split()[1] == "f7g7", "mate move after stop")

        engine.send("ucinewgame")
        engine.send("position startpos")
        engine.send("go depth 3")
        engine.send("isready")
        # Both replies may arrive in either order, but each must occupy its own line.
        ready = best = False
        while not (ready and best):
            line = engine.read_line()
            check(line == "readyok" or line.startswith(("info ", "bestmove ")), f"interleaved protocol output: {line}")
            ready |= line == "readyok"
            best |= line.startswith("bestmove ")
    print("UCI regressions ok")


if __name__ == "__main__":
    main()
