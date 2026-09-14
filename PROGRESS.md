# Aether Chess Progress

Last updated: 2026-09-13 (Asia/Karachi)

## Current implementation

Aether 0.2 implements the September review fixes and the first evaluation/performance improvements:

- Reliable UCI cancellation, joined worker lifecycle, atomic position commands, synchronized output, validated limits, and a real zero-clock deadline.
- FEN structural checks for kings, pawn ranks/counts, castling pieces, counters, and en passant geometry.
- Checkmate precedence over draw claims; correct repetition identity for irrelevant/pinned en passant.
- Incremental Zobrist hashing with an independent recomputation oracle.
- Exact make/unmake restoration, including piece-list order and repetition state; isolated null-move history.
- TT scores qualified by the move counter and reversible-position history.
- Quiescence handling for terminal positions and quiet promotions; legal-attacker SEE used for ordering.
- Staged move selection, exact node limits, final search accounting, and cleared ponder state.
- Tapered middlegame/endgame evaluation with passed pawns, mobility, king shelter, and rook files.
- Castling SAN check/mate suffixes.
- Deadline-bound benchmarks, a tactical suite, paired matches with independent rules, JSON/PGN records, and CI configuration.

## Validation

- Release CTest: all 6 suites pass.
- ASan/UBSan CTest: all 6 suites pass.
- Position regressions include 41,585 transition checks and 3,991 rejected moves.
- Evaluation regressions include 2,513 checks, including 500 legal-playout positions.
- Tactical/terminal suite: all 8 positions pass at 50,000 nodes.
- Tooling regressions exercise a real stalled UCI subprocess, watchdog cleanup, unchanged search limits, draw adjudication, and statistical edge cases.
- Independent python-chess differential perft: 40 seeded positions matched at depth 2.
- Existing Stockfish-confirmed perft expectations remain unchanged.

The final benchmark and match artifacts are written under build/. The match JSON includes executable hashes so a result can be associated with a specific binary.

## Final local comparison

Final executable SHA-256: 3987e84429719a2e68e87ff5734197ddf0727d893f8c1a0f50c1069ae3d59a22.

Against the saved 0.1 executable, the final 12 opening pairs at 25 ms per move finished with **16 wins, 2 draws, and 6 losses** for 0.2 (70.8% score). All 24 PGNs replay legally. There were no engine forfeits or capped draws. Mean measured move time was 20.36 ms for 0.2 and 20.26 ms for 0.1; maxima were 32.33 ms and 28.34 ms, within the declared 45 ms move-plus-overhead allowance. The original engine emitted stale-ponder diagnostics; its played moves remained legal.

This is a short development match. The pair-bootstrap score interval is wide (approximately 52.1%–87.5%), and the sample is insufficient for a reliable Elo claim. Results and move histories are in build/match-results.json and build/match-games.pgn.

Fixed-depth-6 medians across three repetitions, measured in milliseconds:

| Position | Aether 0.2 | Aether 0.1 |
|---|---:|---:|
| Start | 11.92 | 7.36 |
| Open game | 58.21 | 51.66 |
| Tactical | 356.52 | 122.44 |
| Endgame | 7.24 | 5.49 |

The new evaluator and conservative tactical search cost more time at equal nominal depth. These figures are preserved in build/benchmark-depth6.json; the separate 250 ms Stockfish comparison is in build/stockfish-comparison.json. Subsequent performance work should profile this tradeoff while retaining the correctness fixes.

## Reproduce

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAETHER_SANITIZERS=ON
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
build/bench-venv/bin/python tests/tooling_regressions.py
build/bench-venv/bin/python tests/differential_perft.py build/aether --output build/differential-perft.json
build/bench-venv/bin/python scripts/run_suite.py --nodes 50000 --output build/tactical-results.json
build/bench-venv/bin/python scripts/run_matches.py --baseline build/aether-baseline --pairs 12 --movetime 25 --output build/match-results.json --pgn build/match-games.pgn
~~~

See README.md for the optional benchmark dependency setup and code explanation. The original executable is retained at build/aether-baseline; build/review-backup-path.txt identifies the source backup.

## Practical limits and next experiments

- Evaluation weights are handcrafted and untuned. Use paired matches before accepting new weight changes.
- The fuller evaluator and conservative search add work. Fixed-depth tactical searches can be slower even after incremental hashing and staged ordering.
- Node totals are not directly comparable to 0.1 because horizon nodes were previously counted twice.
- Small matches are smoke evidence. Expand openings, game counts, and time controls for a reliable strength estimate.
- Profile legal-move checks at quiescence leaves, SEE, and evaluation before optimizing further.
- Trial aspiration windows and history-aware reductions separately, preserving correctness tests and comparing against a saved baseline.
- NNUE/training data, tablebases, opening books, multiple search threads, and advanced tuning remain future work.
