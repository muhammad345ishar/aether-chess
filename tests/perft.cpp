#include "engine.hpp"

#include <iostream>

int main() {
    using namespace aether;
    Position start;
    const std::uint64_t expected[] = {1, 20, 400, 8902, 197281};
    for (int depth = 0; depth <= 4; ++depth) {
        Position copy = start;
        const auto got = perft(copy, depth);
        if (got != expected[depth]) {
            std::cerr << "startpos perft depth " << depth << ": expected " << expected[depth] << ", got " << got << '\n';
            return 1;
        }
    }
    Position kiwipete("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    const std::uint64_t kiwipeteExpected[] = {1, 48, 2039, 97862};
    for (int depth = 0; depth <= 3; ++depth) {
        Position copy = kiwipete;
        const auto got = perft(copy, depth);
        if (got != kiwipeteExpected[depth]) {
            std::cerr << "kiwipete perft depth " << depth << ": expected " << kiwipeteExpected[depth] << ", got " << got << '\n';
            return 1;
        }
    }
    Position pos3("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    const std::uint64_t pos3Expected[] = {1, 14, 191, 2812, 43238};
    for (int depth = 0; depth <= 4; ++depth) {
        Position copy = pos3;
        const auto got = perft(copy, depth);
        if (got != pos3Expected[depth]) {
            std::cerr << "pos3 perft depth " << depth << ": expected " << pos3Expected[depth] << ", got " << got << '\n';
            return 1;
        }
    }

    struct ReferencePosition {
        const char* name;
        const char* fen;
        std::uint64_t depth3;
    };
    const ReferencePosition references[] = {
        {"promotion tactics", "rnbq1k1r/pp1Pbppp/2p2n2/8/2B5/8/PPP1NPPP/RNBQK2R b KQ - 1 8", 39764},
        {"middlegame", "r4rk1/1pp1qppp/p1np1n2/8/2B1P1b1/2N2N2/PPP1QPPP/2KR3R w - - 0 10", 79591},
    };
    for (const auto& reference : references) {
        Position position(reference.fen);
        const auto got = perft(position, 3);
        if (got != reference.depth3) {
            std::cerr << reference.name << " perft depth 3: expected " << reference.depth3 << ", got " << got << '\n';
            return 1;
        }
    }

    Position ep("8/8/8/Pp6/8/8/8/K6k w - b6 0 1");
    const std::string originalFen = ep.fen();
    for (const Move& move : ep.legalMoves(false)) {
        Undo undo;
        (void)ep.makeMove(move, undo);
        ep.unmakeMove(move, undo);
        if (ep.fen() != originalFen) {
            std::cerr << "make/unmake failed for " << move.uci() << '\n';
            return 1;
        }
    }
    if (!ep.consistent()) {
        std::cerr << "position bookkeeping is inconsistent\n";
        return 1;
    }
    std::cout << "perft ok\n";
    return 0;
}
