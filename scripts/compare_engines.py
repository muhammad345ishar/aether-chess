#!/usr/bin/env python3
"""Repeatable search benchmarks; use run_matches.py to measure playing strength."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import time
from contextlib import ExitStack
from pathlib import Path

from uci_client import UciEngine

POSITIONS = [
    ("start", "startpos"),
    ("open game", "fen r1bqk2r/pppp1ppp/2n2n2/2b1p3/4P3/2NP1N2/PPP2PPP/R1BQKB1R w KQkq - 4 5"),
    ("tactical", "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"),
    ("endgame", "fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"),
]


def analyze(engine: UciEngine, position: str, limit: str) -> dict:
    engine.send("ucinewgame")
    engine.send("isready")
    engine.read_until("readyok")
    engine.send(f"position {position}")
    started = time.monotonic()
    engine.send(f"go {limit}")
    lines = engine.read_until("bestmove")
    result = {"bestmove": lines[-1].split()[1], "wall_ms": round((time.monotonic() - started) * 1000, 3)}
    # Aether emits a final summary including an interrupted iteration's work.
    # Older engines may report only their last completed iteration; wall_ms is
    # measured independently and is always retained for fair timing analysis.
    info = next((line for line in reversed(lines) if line.startswith("info ") and " depth " in f" {line} "), "")
    for field in ("depth", "seldepth", "nodes", "nps", "time"):
        match = re.search(rf"\b{field} (\d+)", info)
        result[field] = int(match.group(1)) if match else 0
    score = re.search(r"\bscore (cp|mate) (-?\d+)", info)
    result["score"] = f"{score.group(1)} {score.group(2)}" if score else "?"
    return result


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aether", type=Path, default=project / "build/aether")
    parser.add_argument("--reference", "--stockfish", dest="reference", type=Path, default=Path("/usr/local/bin/stockfish"))
    limits = parser.add_mutually_exclusive_group()
    limits.add_argument("--movetime", type=int, help="milliseconds per position (default 500)")
    limits.add_argument("--depth", type=int)
    limits.add_argument("--nodes", type=int)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--hash", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--output", type=Path, help="write machine-readable results")
    args = parser.parse_args()
    kind, value = ("depth", args.depth) if args.depth is not None else ("nodes", args.nodes) if args.nodes is not None else ("movetime", 500 if args.movetime is None else args.movetime)
    if value <= 0 or args.repeat <= 0 or not 1 <= args.hash <= 1024 or args.timeout <= 0:
        parser.error("limits, repeat and timeout must be positive; Hash must be 1..1024")
    paths = [args.aether.resolve(), args.reference.resolve()]
    report = {
        "limit": {kind: value}, "hash_mb": args.hash, "repeat": args.repeat,
        "engines": [{"name": name, "path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                    for name, path in zip(("Aether", "Reference"), paths)],
        "results": [],
    }
    with ExitStack() as stack:
        engines = [(name, stack.enter_context(UciEngine(path, args.timeout))) for name, path in zip(("Aether", "Reference"), paths)]
        for _, engine in engines:
            engine.send(f"setoption name Hash value {args.hash}")
            engine.send("isready")
            engine.read_until("readyok")
        print(f"Search comparison: {kind} {value}, Hash {args.hash} MiB, {args.repeat} repetition(s)")
        print(f"{'Position':<12} {'Engine':<10} {'Move':<7} {'Depth':>5} {'Nodes':>11} {'Wall ms':>10} {'Score':>10}", flush=True)
        for repetition in range(args.repeat):
            for label, position in POSITIONS:
                for name, engine in engines[::1 if repetition % 2 == 0 else -1]:
                    result = analyze(engine, position, f"{kind} {value}")
                    report["results"].append({"repeat": repetition + 1, "position": label, "engine": name, **result})
                    print(f"{label:<12} {name:<10} {result['bestmove']:<7} {result['depth']:>5} {result['nodes']:>11} {result['wall_ms']:>10.2f} {result['score']:>10}", flush=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
