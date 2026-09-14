# Aether Chess

Aether 0.2 is a UCI chess engine written in C++17. It has a handcrafted evaluator, single-threaded search, independent rules checks, and a reproducible comparison workflow. Development status and validation results are recorded in [PROGRESS.md](PROGRESS.md).

## Build and run

A C++17 compiler and CMake 3.16 or newer are required. Python 3 enables the executable-level UCI tests.

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/aether
~~~

Example UCI session:

~~~text
uci
isready
position startpos
go depth 8
~~~

Supported search limits: depth, nodes, movetime, wtime/btime, winc/binc, movestogo, and infinite. Hash and Clear Hash are available as UCI options. Invalid positions and malformed or unsupported search limits produce an info-string diagnostic. An invalid position command leaves the previous board intact. Infinite analysis waits for stop before returning bestmove, even when it finds mate immediately.

## How the code works

- **Position and rules — src/position.cpp.** A 0x88 board uses 128 slots with ranks 16 slots apart, making off-board checks inexpensive. Piece lists support move generation. Candidates are made and rejected if they expose the mover's king. Undo saves the information needed to restore the exact board, piece-list order, clocks, and repetition history.
- **Position identity.** Piece moves XOR only the changed squares into the Zobrist hash; castling, side-to-move, and en passant state are updated alongside them. An en passant target contributes only if a legal capture exists, including pin constraints. The full-board hash calculation remains an independent diagnostic oracle. A separate fingerprint of reversible history qualifies cached search scores; it is reset at irreversible moves and isolated across synthetic null moves.
- **Search — src/search.cpp.** Iterative deepening repeatedly searches one ply further. Negamax/PVS explores alternatives, while the transposition table reuses scores only when position, draw clock, and repetition context agree. Moves are selected in stages: cached move, favorable captures/promotions, killers, history-ranked quiets, and remaining captures. SEE estimates exchanges using legal recaptures and influences ordering.
- **Tactical leaves.** Quiescence searches captures, all promotions, and check evasions. It checks terminal positions before treating the static evaluation as a valid score. Checkmate takes precedence over move-count draws. Repetition cycles are treated as draws within search; the game-result API requires an actual third occurrence.
- **Evaluation — src/evaluation.cpp.** Each feature has middlegame and endgame weights. Remaining knights/bishops count as one phase unit, rooks as two, and queens as four, with 24 units initially. The blend is (middlegame × phase + endgame × (24 − phase)) / 24. Features include material, piece-square placement, bishop pair, pawn structure, passed pawns, pawn-safe mobility, king shelter, and rook files. Endgame king placement rewards activity. These weights are handcrafted and have not been statistically tuned.
- **UCI lifecycle — src/main.cpp.** prepareSearch() clears cancellation before the worker is launched. The worker never resets it on entry, so an immediate stop survives. Commands that change state stop and join the previous worker first. Protocol output is serialized, and position commands are committed only after every FEN field and move has been accepted.
- **Notation — src/notation.cpp.** UCI/SAN parsing, SAN output including castling check/mate suffixes, perft, and per-move diagnostics live here.

The result API adjudicates current fifty-move and threefold claims as draws. It is not an interactive draw-claim UI. FEN validation checks structural invariants; it does not prove historical reachability. makeMove expects generated pseudo-legal moves; parseMove is the entry point for external move text.

## Regression and sanitizer checks

CTest includes the existing reference perft/core suites plus position, search, evaluation, and actual UCI process regressions. They cover malformed FENs, en passant pins, promotions, repetition, null moves, mate at the fifty-move boundary, TT context, cancellation, and seeded random make/unmake paths.

~~~sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAETHER_SANITIZERS=ON
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
~~~

The GitHub Actions workflow runs release-with-debug-info builds on Linux and macOS with and without AddressSanitizer/UndefinedBehaviorSanitizer.

## Search benchmarks

The comparison script needs only the Python standard library. It enforces response deadlines, alternates engine order across repetitions, measures elapsed wall time, and can save JSON including executable hashes.

~~~sh
python3 scripts/compare_engines.py --movetime 250 --output build/stockfish-comparison.json
python3 scripts/compare_engines.py --reference build/aether-baseline --depth 6 --repeat 3 --output build/depth-comparison.json
~~~

The default reference path is /usr/local/bin/stockfish; override it with --reference. The old --stockfish spelling still works. --nodes is also supported. Raw NPS differs across versions because 0.2 counts the negamax-to-quiescence transition once. Changes to evaluation and pruning also change the searched tree, so compare fixed-depth time, fixed-time decisions, and match results separately.

The original pre-improvement executable is saved locally as build/aether-baseline. The source-backup location is recorded in build/review-backup-path.txt. For subsequent experiments, preserve a known executable before rebuilding.

## Tactical suite and paired games

These optional tools use python-chess as an independent move validator and adjudicator:

~~~sh
python3 -m venv build/bench-venv
build/bench-venv/bin/python -m pip install -r scripts/requirements.txt
build/bench-venv/bin/python tests/tooling_regressions.py
build/bench-venv/bin/python tests/differential_perft.py build/aether
build/bench-venv/bin/python scripts/run_suite.py --nodes 50000 --output build/tactical-results.json
build/bench-venv/bin/python scripts/run_matches.py --baseline build/aether-baseline --pairs 12 --movetime 25 --output build/match-results.json --pgn build/match-games.pgn
~~~

tests/positions.json contains tactical and terminal-position expectations. tests/openings.json contains 12 opening lines; each is played with colors reversed. To run more pairs, provide a larger JSON opening book with --openings. Books contain objects with a name and either a FEN, a list of UCI moves, or both.

Match JSON records executable hashes, limits, seed, timing, game results, pair-level bootstrap confidence intervals, and any engine failure. PGN stores all played moves. The response watchdog also covers node-limited searches. For timed games, a move exceeding movetime plus --overhead-ms (default 20 ms for position transfer and process overhead) forfeits; such failures stop the run and preserve its partial report. The slower response watchdog is a separate safeguard for a hung process.

Adjudication checks mate/stalemate first, then current fifty-move/threefold draws. It does not assume that a player will choose a future drawing move instead of a winning move. Games reaching --max-plies are recorded explicitly as capped draws. Confidence intervals resample entire opening pairs; they are omitted for insufficient, degenerate, or incomplete samples.

Small smoke matches provide development evidence, not a reliable Elo rating. Increase the opening set, game count, and time control before making strength claims. NNUE, tablebases, automated tuning, and multiple search threads remain future work.
