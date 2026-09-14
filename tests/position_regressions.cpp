#include "engine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace aether;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "position regression: " << message << '\n';
        std::exit(1);
    }
}

struct Snapshot {
    std::string fen;
    std::uint64_t key;
    std::uint64_t context;
    int ply;
    bool repeatedOnce;
    bool repeatedTwice;
    std::array<std::vector<int>, 2> lists;

    explicit Snapshot(const Position& position)
        : fen(position.fen()), key(position.key()), context(position.repetitionContext()),
          ply(position.gamePly()), repeatedOnce(position.isRepetition(1)), repeatedTwice(position.isRepetition()) {
        for (int color : {WHITE, BLACK}) {
            for (int i = 0; i < position.pieceCount(color); ++i) lists[color].push_back(position.pieceSquare(color, i));
        }
    }

    void check(const Position& position, const std::string& label) const {
        require(position.consistent(), label + ": hash, history, or piece-list invariant");
        require(position.fen() == fen && position.key() == key && position.repetitionContext() == context &&
                position.gamePly() == ply && position.isRepetition(1) == repeatedOnce &&
                position.isRepetition() == repeatedTwice, label + ": state restoration");
        for (int color : {WHITE, BLACK}) {
            require(position.pieceCount(color) == static_cast<int>(lists[color].size()), label + ": piece count");
            for (int i = 0; i < position.pieceCount(color); ++i) {
                require(position.pieceSquare(color, i) == lists[color][static_cast<std::size_t>(i)], label + ": piece-list order");
            }
        }
    }
};

Move play(Position& position, const std::string& text, Undo& undo) {
    const Move move = parseMove(position, text);
    require(!move.isNull(), "parse " + text + " in " + position.fen());
    require(position.makeMove(move, undo), "make " + text);
    require(position.consistent(), "consistent after " + text);
    return move;
}

void play(Position& position, const std::string& text) {
    Undo undo;
    (void)play(position, text, undo);
}

void fenValidation() {
    Position position;
    for (const char* move : {"e2e4", "g8f6", "g1f3", "f6g8", "f3g1"}) play(position, move);
    const Snapshot original(position);
    const char* invalid[] = {
        "P3k3/8/8/8/8/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/8/8/8/p3K3 w - - 0 1",
        "4k3/8/8/8/8/8/8/3KK3 w - - 0 1",
        "3kk3/8/8/8/8/8/8/4K3 w - - 0 1",
        "8/8/8/8/8/8/8/4K3 w - - 0 1",
        "8/8/8/8/8/8/4k3/4K3 w - - 0 1",
        "4k3/8/8/8/8/P7/PPPPPPPP/4K3 w - - 0 1",
        "4k3/8/3n4/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/3N4/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/3PP3/8/8/8/4K3 w - d6 0 1",
        "4k3/3p4/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d3 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - z6 0 1",
        "4k3/8/8/8/8/8/8/4K3 w K - 0 1",
        "4k3/8/8/8/8/8/8/4K2R w KK - 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - - bad 1",
        "4k3/8/8/8/8/8/8/4K3 w - - 1x 1",
        "4k3/8/8/8/8/8/8/4K3 w - - -1 1",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 0",
        "4k3/8/8/8/8/8/8/4K3 w - - 2147483648 1",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1 extra",
    };
    for (const char* fen : invalid) {
        require(!position.setFen(fen), std::string("accepted invalid FEN: ") + fen);
        original.check(position, "rejected FEN");
    }
    require(position.setFen("4k3/8/8/8/8/8/8/4K3 w - -"), "four-field FEN compatibility");
    require(position.fen() == "4k3/8/8/8/8/8/8/4K3 w - - 0 1", "default FEN counters");
    Position counters("4k3/8/8/8/8/8/8/4K3 b - - 2147483647 2147483647");
    const Snapshot counterState(counters);
    Undo undo;
    const Move move = play(counters, "e8d8", undo);
    require(counters.halfmoveClock() == 2147483647 && counters.fullmoveNumber() == 2147483647, "counter saturation");
    counters.unmakeMove(move, undo);
    counterState.check(counters, "counter unmake");
}

void epHashing() {
    struct Case { const char* fen; const char* move; bool legal; };
    const Case cases[] = {
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6", true},
        {"4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1", "e4d3", true},
        {"4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6", false},
        {"7k/8/8/r4pPK/8/8/8/8 w - f6 0 1", "g5f6", false},
        {"4r1k1/8/8/2PpP3/8/8/8/4K3 w - d6 0 1", "c5d6", true},
        {"4k3/8/8/3pP3/4K3/8/8/8 w - d6 0 1", "e5d6", true},
        {"4k3/8/8/3p4/8/8/8/4K3 w - d6 0 1", "e5d6", false},
    };
    for (const Case& item : cases) {
        Position position;
        require(position.setFen(item.fen), std::string("EP fixture: ") + item.fen);
        require(position.consistent(), "EP initial consistency");
        std::string noEp = position.fen();
        const std::string square = squareName(position.epSquare());
        noEp.replace(noEp.find(" " + square + " ") + 1, 2, "-");
        Position without(noEp);
        require((position.key() != without.key()) == item.legal, "EP hash must reflect a legal capture");
        require((!parseMove(position, item.move).isNull()) == item.legal, "EP legality");
        const Snapshot original(position);
        Undo undo;
        position.makeNullMove(undo);
        require(position.consistent() && position.epSquare() == -1, "null removes EP hash");
        position.unmakeNullMove(undo);
        original.check(position, "null restores EP hash");
    }

    Position repeated;
    play(repeated, "e2e4");
    const std::uint64_t afterPawnPush = repeated.key();
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (const char* move : {"g8f6", "g1f3", "f6g8", "f3g1"}) play(repeated, move);
        require(repeated.key() == afterPawnPush, "irrelevant EP target changed repetition identity");
        require(repeated.isRepetition(1), "first cycle repeats");
        require(repeated.isRepetition() == (cycle == 1), "threefold occurrence count");
    }
    require(repeated.result() == GameResult::DrawRepetition, "threefold game result");
    const Snapshot beforeNull(repeated);
    Undo nullA, nullB;
    repeated.makeNullMove(nullA);
    repeated.makeNullMove(nullB);
    require(repeated.key() == beforeNull.key && repeated.consistent(), "two nulls restore board hash");
    require(!repeated.isRepetition(1), "null barrier excludes game history");
    require(repeated.gamePly() == beforeNull.ply, "null move must not append game history");
    struct Step { Move move; Undo undo; };
    std::vector<Step> steps;
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (const char* text : {"g8f6", "g1f3", "f6g8", "f3g1"}) {
            Undo undo;
            const Move move = play(repeated, text, undo);
            steps.push_back({move, undo});
        }
        require(repeated.isRepetition(1) == (cycle == 1), "only legal history after null contributes");
        require(!repeated.isRepetition(), "synthetic null state cannot become a third occurrence");
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) repeated.unmakeMove(it->move, it->undo);
    repeated.unmakeNullMove(nullB);
    repeated.unmakeNullMove(nullA);
    beforeNull.check(repeated, "nested null/history restoration");
}

void terminalAndNotation() {
    Position mate("7k/6Q1/5K2/8/8/8/8/8 b - - 100 1");
    require(mate.result() == GameResult::WhiteWins, "white checkmate precedes fifty-move draw");
    Position blackMate("8/8/8/8/8/5k2/6q1/7K w - - 100 1");
    require(blackMate.result() == GameResult::BlackWins, "black checkmate precedes fifty-move draw");
    Position matingMove("7k/8/5KQ1/8/8/8/8/8 w - - 99 1");
    play(matingMove, "g6g7");
    require(matingMove.halfmoveClock() == 100 && matingMove.result() == GameResult::WhiteWins, "hundredth halfmove can mate");
    struct Castle { const char* fen; const char* move; const char* san; };
    const Castle castles[] = {
        {"5k2/8/8/8/8/8/8/4K2R w K - 0 1", "e1g1", "O-O+"},
        {"3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", "e1c1", "O-O-O+"},
        {"4k2r/8/8/8/8/8/8/5K2 b k - 0 1", "e8g8", "O-O+"},
        {"5k2/2N1N3/8/3N3N/8/8/8/4K2R w K - 0 1", "e1g1", "O-O#"},
    };
    for (const Castle& item : castles) {
        Position position(item.fen);
        const Move move = parseMove(position, item.move);
        require(!move.isNull() && toSan(position, move) == item.san, std::string("castling SAN ") + item.san);
        require(parseMove(position, item.san) == move, "castling SAN round trip");
    }
}

void repetitionContext() {
    Position first, second;
    const std::array<const char*, 4> kingKnight = {"g1f3", "g8f6", "f3g1", "f6g8"};
    const std::array<const char*, 4> queenKnight = {"b1c3", "b8c6", "c3b1", "c6b8"};
    for (const char* move : kingKnight) play(first, move);
    for (const char* move : queenKnight) play(first, move);
    for (const char* move : queenKnight) play(second, move);
    for (const char* move : kingKnight) play(second, move);
    require(first.fen() == second.fen() && first.key() == second.key(), "equivalent final positions");
    require(first.repetitionContext() == second.repetitionContext(), "history multiset is order-independent");
    require(first.isRepetition() && second.isRepetition(), "equivalent history occurrence counts");
    Position fresh(first.fen());
    require(fresh.key() == first.key() && fresh.repetitionContext() != first.repetitionContext(), "fresh FEN has distinct draw history");
    play(first, "e2e4");
    play(fresh, "e2e4");
    require(first.repetitionContext() == fresh.repetitionContext(), "pawn move resets reversible history");

    Position rook("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    play(rook, "h1h2");
    Position rookFresh(rook.fen());
    require(rook.repetitionContext() == rookFresh.repetitionContext(), "castling-rights loss resets reversible history");
}

void promotionsAndSee() {
    for (const char* fen : {"7k/P7/8/8/8/8/8/6K1 w - - 0 1", "6k1/8/8/8/8/8/p7/7K b - - 0 1"}) {
        Position position(fen);
        MoveList tactical;
        position.generate(tactical, true);
        require(tactical.size() == 4, "tactical generator includes all four quiet promotions");
        require(position.legalMoves(true).size() == 4, "legal tactical generator includes quiet promotions");
        std::array<bool, 7> found{};
        for (const Move& move : tactical) {
            require(move.isPromotion() && !move.isCapture(), "quiet promotion flags");
            found[move.promotionType()] = true;
        }
        for (int type = KNIGHT; type <= QUEEN; ++type) require(found[type], "promotion choice missing");
    }
    struct Exchange { const char* fen; const char* move; int expected; };
    const Exchange exchanges[] = {
        {"3k4/8/2pr4/1B6/8/8/8/3R2K1 w - - 0 1", "b5c6", pieceValue(PAWN)},
        {"8/8/4k3/3p4/2B5/8/8/3R2K1 w - - 0 1", "c4d5", pieceValue(PAWN)},
        {"8/8/4k3/3p4/2B5/8/8/6K1 w - - 0 1", "c4d5", pieceValue(PAWN) - pieceValue(BISHOP)},
        {"1r6/P7/7k/8/8/8/8/6K1 w - - 0 1", "a7a8q", -pieceValue(PAWN)},
        {"1r6/P7/7k/8/8/8/8/6K1 w - - 0 1", "a7b8q", pieceValue(ROOK) + pieceValue(QUEEN) - pieceValue(PAWN)},
    };
    for (const Exchange& item : exchanges) {
        Position position(item.fen);
        const Snapshot original(position);
        const Move move = parseMove(position, item.move);
        require(!move.isNull(), "SEE fixture move");
        require(position.see(move) == item.expected, std::string("SEE ") + item.move + ": got " + std::to_string(position.see(move)));
        original.check(position, "SEE does not mutate position");
    }
}

void randomRestoration() {
    const char* fixtures[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        "4k2r/6P1/8/8/8/8/1p6/R3K3 w Qk - 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "7k/8/8/r4pPK/8/8/8/8 w - f6 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    };
    std::mt19937 rng(0xAE7E2026U);
    std::array<bool, 16> flags{};
    int rejected = 0;
    int checked = 0;
    for (const char* fen : fixtures) {
        for (int game = 0; game < 3; ++game) {
            Position position;
            require(position.setFen(fen), "random fixture FEN");
            const Snapshot initial(position);
            struct Step { Snapshot before; Move move; Undo undo; };
            std::vector<Step> path;
            for (int ply = 0; ply < 100; ++ply) {
                const Snapshot before(position);
                MoveList moves;
                position.generate(moves);
                std::vector<Move> legal;
                for (const Move& move : moves) {
                    Undo undo;
                    if (position.makeMove(move, undo)) {
                        require(position.consistent(), "random make/hash oracle " + move.uci());
                        flags[move.flag] = true;
                        legal.push_back(move);
                        position.unmakeMove(move, undo);
                    } else ++rejected;
                    before.check(position, "random legal/rejected unmake " + move.uci());
                    ++checked;
                }
                if (legal.empty()) break;
                if (!position.inCheck() && rng() % 5 == 0) {
                    Undo nullUndo;
                    position.makeNullMove(nullUndo);
                    require(position.consistent() && !position.isRepetition(1), "random null consistency/barrier");
                    require(position.halfmoveClock() == nullUndo.halfmove, "null cannot advance draw clock");
                    const Snapshot nullState(position);
                    const auto replies = position.legalMoves();
                    if (!replies.empty()) {
                        const Move reply = replies[rng() % replies.size()];
                        Undo replyUndo;
                        require(position.makeMove(reply, replyUndo) && position.consistent(), "random child of null");
                        position.unmakeMove(reply, replyUndo);
                        nullState.check(position, "null child unmake");
                    }
                    position.unmakeNullMove(nullUndo);
                    before.check(position, "random null unmake");
                }
                const Move move = legal[rng() % legal.size()];
                Undo undo;
                require(position.makeMove(move, undo), "random selected move");
                path.push_back({before, move, undo});
            }
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                position.unmakeMove(it->move, it->undo);
                it->before.check(position, "random reverse path");
            }
            initial.check(position, "random complete history restoration");
        }
    }
    for (int flag : {QUIET, DOUBLE_PUSH, KING_CASTLE, QUEEN_CASTLE, CAPTURE, EP_CAPTURE,
                     PROMOTE_N, PROMOTE_B, PROMOTE_R, PROMOTE_Q,
                     PROMOTE_N_CAPTURE, PROMOTE_B_CAPTURE, PROMOTE_R_CAPTURE, PROMOTE_Q_CAPTURE}) {
        require(flags[flag], "special move missing from randomized coverage: " + std::to_string(flag));
    }
    require(rejected > 0 && checked > 10000, "randomized legality/restoration coverage");
    std::cout << "position regressions ok (" << checked << " transitions, " << rejected << " rejected moves)\n";
}

} // namespace

int main() {
    fenValidation();
    epHashing();
    terminalAndNotation();
    repetitionContext();
    promotionsAndSee();
    randomRestoration();
    return 0;
}
