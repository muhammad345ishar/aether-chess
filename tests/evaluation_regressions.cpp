#include "engine.hpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {

using namespace aether;

int checks = 0;

void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Position positionFrom(const std::string& fen) {
    Position position;
    if (!position.setFen(fen)) throw std::runtime_error("invalid test FEN: " + fen);
    return position;
}

int score(const std::string& fen) {
    return evaluate(positionFrom(fen));
}

// Evaluation depends on the board and side, so omit castling/en-passant state
// when testing a turn flip. Reflect ranks, not files: king-side and queen-side
// piece-square preferences need not be mirror images of one another.
Position transformed(const Position& source, bool reflectColors, bool flipTurn) {
    constexpr char PIECES[] = ".PNBRQKpnbrqk";
    std::string fen;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const int fromRank = reflectColors ? 7 - rank : rank;
            int piece = source.pieceAt(squareOf(file, fromRank));
            if (piece == EMPTY) {
                ++empty;
                continue;
            }
            if (empty != 0) fen += static_cast<char>('0' + empty);
            empty = 0;
            if (reflectColors) piece = makePiece(colorOf(piece) ^ 1, typeOf(piece));
            fen += PIECES[piece];
        }
        if (empty != 0) fen += static_cast<char>('0' + empty);
        if (rank != 0) fen += '/';
    }
    const int side = source.sideToMove() ^ static_cast<int>(flipTurn);
    fen += side == WHITE ? " w - - 0 1" : " b - - 0 1";
    return positionFrom(fen);
}

void expectSymmetry(const Position& position) {
    const int original = evaluate(position);
    expect(original == -evaluate(transformed(position, false, true)),
           "turn flip must negate evaluation: " + position.fen());
    expect(original == evaluate(transformed(position, true, true)),
           "color/rank reflection must preserve the side-to-move score: " + position.fen());
    expect(original == -evaluate(transformed(position, true, false)),
           "color/rank reflection with unchanged turn must negate evaluation: " + position.fen());
    expect(std::abs(original) < VALUE_MATE_IN_MAX_PLY,
           "static evaluation entered the mate-score range: " + position.fen());
}

void testSymmetry() {
    Position start;
    expect(evaluate(start) == 0, "the starting position must evaluate equally");
    const char* seeds[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQ1RK1 b kq - 5 5",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "8/5pk1/4p1p1/3pP3/3P1P2/6P1/5K2/8 w - - 0 1"
    };
    std::mt19937 random(0xa37e2026U);
    for (const char* fen : seeds) {
        Position position = positionFrom(fen);
        for (int ply = 0; ply < 100; ++ply) {
            expectSymmetry(position);
            const auto moves = position.legalMoves();
            if (moves.empty()) break;
            const Move move = moves[random() % moves.size()];
            Undo undo;
            expect(position.makeMove(move, undo), "legal playout move was rejected");
        }
    }
    // Promotion-rich material must still produce an ordinary static score,
    // including when the side to move is far behind.
    expectSymmetry(positionFrom("7k/8/8/8/8/NNBBRR2/QQQQQQQQ/KQ6 b - - 0 1"));
}

void testKingPhase() {
    const int endgameHome = score("7k/7p/8/8/8/8/P7/6K1 w - - 0 1");
    const int endgameActive = score("7k/7p/8/8/3K4/8/P7/8 w - - 0 1");
    expect(endgameActive > endgameHome, "an endgame king should benefit from central activity");

    const int middlegameHome = score(startFen());
    const int middlegameActive = score("rnbqkbnr/pppppppp/8/8/3K4/8/PPPPPPPP/RNBQ1BNR w - - 0 1");
    expect(middlegameHome > middlegameActive, "a middlegame king should prefer its sheltered home");

    // Moving the same three pawns changes their own positional score in both
    // pairs. The extra loss with Kg1 isolates the benefit of its close shield.
    const int closeShield = score("k2q4/8/8/8/8/8/5PPP/3Q2K1 w - - 0 1")
                          - score("k2q4/8/8/8/8/8/5PPP/2KQ4 w - - 0 1");
    const int advancedShield = score("k2q4/8/8/8/8/5PPP/8/3Q2K1 w - - 0 1")
                             - score("k2q4/8/8/8/8/5PPP/8/2KQ4 w - - 0 1");
    expect(closeShield > advancedShield, "nearby shield pawns should protect the king better");
}

void testPassedPawns() {
    const int thirdRank = score("7k/8/7p/8/8/P7/8/6K1 w - - 0 1");
    const int fifthRank = score("7k/8/7p/P7/8/8/8/6K1 w - - 0 1");
    const int seventhRank = score("7k/P7/7p/8/8/8/8/6K1 w - - 0 1");
    expect(thirdRank < fifthRank && fifthRank < seventhRank,
           "an unobstructed passer should become more valuable as it advances");

    const int adjacentEnemy = score("7k/1p6/8/P7/8/8/8/6K1 w - - 0 1");
    const int distantEnemy = score("7k/2p5/8/P7/8/8/8/6K1 w - - 0 1");
    expect(distantEnemy > adjacentEnemy,
           "an enemy pawn ahead on the adjacent file must prevent the passer bonus");
}

void testMobilityAndRookFiles() {
    // Compare the same pawn advance with and without a bishop. The additional
    // benefit measures the bishop's newly available diagonal, not pawn value.
    const int bishopOpens = score("k7/8/8/8/8/3P4/8/2B4K w - - 0 1")
                          - score("k7/8/8/8/8/8/3P4/2B4K w - - 0 1");
    const int pawnOnly = score("k7/8/8/8/8/3P4/8/7K w - - 0 1")
                       - score("k7/8/8/8/8/8/3P4/7K w - - 0 1");
    expect(bishopOpens > pawnOnly, "opening a bishop diagonal should add a mobility benefit");

    // Na2 blocks Ra1 in every position below, keeping its mobility unchanged.
    // Subtract controls without the rook to separate file access from the
    // pawns' own piece-square/structure changes.
    const int semiOpenRook = score("7k/p7/8/8/8/1P6/N7/R5K1 w - - 0 1")
                           - score("7k/p7/8/8/8/P7/N7/R5K1 w - - 0 1");
    const int semiOpenControl = score("7k/p7/8/8/8/1P6/N7/6K1 w - - 0 1")
                              - score("7k/p7/8/8/8/P7/N7/6K1 w - - 0 1");
    expect(semiOpenRook > semiOpenControl, "a rook should receive a semi-open file bonus");

    const int openRook = score("7k/1p6/8/8/8/1P6/N7/R5K1 w - - 0 1")
                       - score("7k/p7/8/8/8/1P6/N7/R5K1 w - - 0 1");
    const int openControl = score("7k/1p6/8/8/8/1P6/N7/6K1 w - - 0 1")
                          - score("7k/p7/8/8/8/1P6/N7/6K1 w - - 0 1");
    expect(openRook > openControl, "a fully open rook file should improve on a semi-open one");
}

} // namespace

int main() {
    try {
        testSymmetry();
        testKingPhase();
        testPassedPawns();
        testMobilityAndRookFiles();
    } catch (const std::exception& error) {
        std::cerr << "evaluation regression failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "evaluation regressions ok (" << checks << " checks)\n";
    return 0;
}
