#include "engine.hpp"

#include <algorithm>
#include <array>

namespace aether {
namespace {

// Each feature has a middlegame and an endgame weight, in centipawns. These
// are initial handcrafted values; playing-strength tuning needs match data.
struct Score {
    int mg = 0;
    int eg = 0;

    constexpr Score& operator+=(Score other) {
        mg += other.mg;
        eg += other.eg;
        return *this;
    }
};

constexpr Score scaled(Score score, int count) {
    return {score.mg * count, score.eg * count};
}

constexpr Score MATERIAL[7] = {
    {}, {100, 120}, {320, 305}, {335, 325}, {500, 525}, {900, 930}, {}
};
// The starting position has 24 phase units. Pawn moves do not change phase;
// promotions can add phase, so the blend is clamped to the starting maximum.
constexpr int PHASE_WEIGHT[7] = {0, 0, 1, 1, 2, 4, 0};
constexpr int MAX_PHASE = 24;
constexpr Score BISHOP_PAIR = {28, 40};
constexpr Score DOUBLED_PAWN = {-12, -18};
constexpr Score ISOLATED_PAWN = {-8, -12};
constexpr Score ROOK_SEMI_OPEN_FILE = {12, 8};
constexpr Score ROOK_OPEN_FILE = {24, 14};
constexpr Score KING_CLOSE_SHELTER = {12, 0};
constexpr Score KING_ADVANCED_SHELTER = {6, 0};
constexpr Score KING_MISSING_SHELTER = {-12, 0};
constexpr Score MOBILITY[7] = {
    {}, {}, {4, 4}, {5, 5}, {2, 4}, {1, 2}, {}
};
constexpr int MOBILITY_BASE[7] = {0, 0, 4, 6, 7, 12, 0};
constexpr Score PASSED_PAWN[8] = {
    {}, {2, 5}, {6, 12}, {12, 26}, {24, 48}, {42, 85}, {70, 140}, {}
};
constexpr int BLOCKED_PASSER_DIVISOR = 2;
constexpr int KNIGHT_STEPS[] = {31, 33, 14, 18, -31, -33, -14, -18};
constexpr int SLIDER_STEPS[] = {15, 17, -15, -17, 1, -1, 16, -16};

constexpr int MG_PST[7][64] = {
    {},
    {0,0,0,0,0,0,0,0, 5,10,10,-20,-20,10,10,5, 5,-5,-10,0,0,-10,-5,5, 0,0,0,20,20,0,0,0,
     5,5,10,25,25,10,5,5, 10,10,20,30,30,20,10,10, 50,50,50,50,50,50,50,50, 0,0,0,0,0,0,0,0},
    {-50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,5,5,0,-20,-40, -30,5,10,15,15,10,5,-30,
     -30,0,15,20,20,15,0,-30, -30,5,15,20,20,15,5,-30, -30,0,10,15,15,10,0,-30,
     -40,-20,0,0,0,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50},
    {-20,-10,-10,-10,-10,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,10,10,5,0,-10,
     -10,5,5,10,10,5,5,-10, -10,0,10,10,10,10,0,-10, -10,10,10,10,10,10,10,-10,
     -10,5,0,0,0,0,5,-10, -20,-10,-10,-10,-10,-10,-10,-20},
    {0,0,0,5,5,0,0,0, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5,
     -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 5,10,10,10,10,10,10,5, 0,0,0,0,0,0,0,0},
    {-20,-10,-10,-5,-5,-10,-10,-20, -10,0,5,0,0,5,0,-10, -10,5,5,5,5,5,5,-10,
     -5,0,5,5,5,5,0,-5, 0,0,5,5,5,5,0,0, -10,5,5,5,5,5,0,-10,
     -10,0,5,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20},
    {20,30,10,0,0,10,30,20, 20,20,0,0,0,0,20,20, -10,-20,-20,-20,-20,-20,-20,-10,
     -20,-30,-30,-40,-40,-30,-30,-20, -30,-40,-40,-50,-50,-40,-40,-30,
     -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30}
};

// Endgame tables favor central kings and active pieces. The formulas are
// expanded at compile time; evaluating a piece still needs only one lookup.
constexpr int CENTRALITY[8] = {0, 1, 2, 3, 3, 2, 1, 0};
constexpr int EG_PAWN_ADVANCE[8] = {0, 0, 4, 10, 20, 35, 55, 0};
constexpr int EG_PAWN_FILE_WEIGHT = 2;
constexpr int EG_KNIGHT_CENTER_WEIGHT = 10;
constexpr int EG_BISHOP_CENTER_WEIGHT = 5;
constexpr int EG_ROOK_SEVENTH_RANK = 20;
constexpr int EG_QUEEN_CENTER_WEIGHT = 4;
constexpr int EG_KING_CENTER_WEIGHT = 14;

constexpr int endgamePst(int type, int file, int rank) {
    const int center = CENTRALITY[file] + CENTRALITY[rank];
    switch (type) {
    case PAWN: return EG_PAWN_ADVANCE[rank] + EG_PAWN_FILE_WEIGHT * CENTRALITY[file];
    case KNIGHT: return EG_KNIGHT_CENTER_WEIGHT * (center - 3);
    case BISHOP: return EG_BISHOP_CENTER_WEIGHT * (center - 3);
    case ROOK: return rank == 6 ? EG_ROOK_SEVENTH_RANK : 0;
    case QUEEN: return EG_QUEEN_CENTER_WEIGHT * (center - 3);
    case KING: return EG_KING_CENTER_WEIGHT * (center - 3);
    default: return 0;
    }
}

constexpr auto makePieceSquareTable() {
    std::array<std::array<Score, 64>, 7> table{};
    for (int type = PAWN; type <= KING; ++type) {
        for (int rank = 0; rank < 8; ++rank) {
            for (int file = 0; file < 8; ++file) {
                const int index = rank * 8 + file;
                table[type][index] = {MG_PST[type][index], endgamePst(type, file, rank)};
            }
        }
    }
    return table;
}

constexpr auto PIECE_SQUARE = makePieceSquareTable();

int relativeRank(int square, int color) {
    return color == WHITE ? rankOf(square) : 7 - rankOf(square);
}

struct PawnInfo {
    std::array<std::array<int, 8>, 2> counts{};
    // Bit r records a pawn on absolute rank r; this makes passer tests cheap.
    std::array<std::array<unsigned, 8>, 2> ranks{};
    std::array<std::array<bool, 128>, 2> attacks{};
};

int mobility(const Position& position, const PawnInfo& pawns, int square, int type, int color) {
    int count = 0;
    const auto& unsafe = pawns.attacks[color ^ 1];
    if (type == KNIGHT) {
        for (int step : KNIGHT_STEPS) {
            const int to = square + step;
            if (!onBoard(to)) continue;
            const int occupant = position.pieceAt(to);
            if ((occupant == EMPTY || colorOf(occupant) != color) && !unsafe[to]) ++count;
        }
    } else {
        const int begin = type == ROOK ? 4 : 0;
        const int end = type == BISHOP ? 4 : 8;
        for (int i = begin; i < end; ++i) {
            const int step = SLIDER_STEPS[i];
            for (int to = square + step; onBoard(to); to += step) {
                const int occupant = position.pieceAt(to);
                if (occupant != EMPTY && colorOf(occupant) == color) break;
                if (!unsafe[to]) ++count;
                if (occupant != EMPTY) break;
            }
        }
    }
    return count;
}

Score passedPawn(const Position& position, const PawnInfo& pawns, int square, int color) {
    const int rank = rankOf(square);
    const int file = fileOf(square);
    const unsigned ahead = color == WHITE ? (0xffU << (rank + 1)) & 0xffU : (1U << rank) - 1U;
    // A doubled pawn behind another friendly pawn is not an extra passer.
    if ((pawns.ranks[color][file] & ahead) != 0) return {};
    for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); ++f) {
        if ((pawns.ranks[color ^ 1][f] & ahead) != 0) return {};
    }
    Score bonus = PASSED_PAWN[relativeRank(square, color)];
    const int next = square + (color == WHITE ? 16 : -16);
    if (onBoard(next) && position.pieceAt(next) != EMPTY) {
        bonus.mg /= BLOCKED_PASSER_DIVISOR;
        bonus.eg /= BLOCKED_PASSER_DIVISOR;
    }
    return bonus;
}

Score kingShelter(const Position& position, int color) {
    const int king = position.kingSquare(color);
    const int direction = color == WHITE ? 16 : -16;
    // Even a corner king uses three shield files (a/b/c or f/g/h).
    const int centerFile = std::clamp(fileOf(king), 1, 6);
    const int pawn = makePiece(color, PAWN);
    Score score;
    for (int file = centerFile - 1; file <= centerFile + 1; ++file) {
        const int close = squareOf(file, rankOf(king)) + direction;
        const int advanced = close + direction;
        if (onBoard(close) && position.pieceAt(close) == pawn) score += KING_CLOSE_SHELTER;
        else if (onBoard(advanced) && position.pieceAt(advanced) == pawn) score += KING_ADVANCED_SHELTER;
        else score += KING_MISSING_SHELTER;
    }
    return score;
}

} // namespace

int pieceValue(int type) {
    // SEE and capture ordering use this fixed exchange scale, independently
    // of the evaluator's phase-dependent material values.
    static constexpr int values[] = {0, 100, 320, 335, 500, 900, 20000};
    return type >= NO_TYPE && type <= KING ? values[type] : 0;
}

int evaluate(const Position& position) {
    std::array<Score, 2> scores{};
    std::array<int, 2> bishops{};
    PawnInfo pawns;
    int phase = 0;
    for (int color = WHITE; color <= BLACK; ++color) {
        for (int i = 0; i < position.pieceCount(color); ++i) {
            const int square = position.pieceSquare(color, i);
            const int type = typeOf(position.pieceAt(square));
            scores[color] += MATERIAL[type];
            scores[color] += PIECE_SQUARE[type][relativeRank(square, color) * 8 + fileOf(square)];
            phase += PHASE_WEIGHT[type];
            if (type == BISHOP) ++bishops[color];
            if (type == PAWN) {
                ++pawns.counts[color][fileOf(square)];
                pawns.ranks[color][fileOf(square)] |= 1U << rankOf(square);
                const int forward = square + (color == WHITE ? 16 : -16);
                if (onBoard(forward - 1)) pawns.attacks[color][forward - 1] = true;
                if (onBoard(forward + 1)) pawns.attacks[color][forward + 1] = true;
            }
        }
    }
    for (int color = WHITE; color <= BLACK; ++color) {
        if (bishops[color] >= 2) scores[color] += BISHOP_PAIR;
        scores[color] += kingShelter(position, color);
        for (int file = 0; file < 8; ++file) {
            const int count = pawns.counts[color][file];
            if (count > 1) scores[color] += scaled(DOUBLED_PAWN, count - 1);
            if (count > 0) {
                const bool left = file > 0 && pawns.counts[color][file - 1] > 0;
                const bool right = file < 7 && pawns.counts[color][file + 1] > 0;
                if (!left && !right) scores[color] += scaled(ISOLATED_PAWN, count);
            }
        }
        for (int i = 0; i < position.pieceCount(color); ++i) {
            const int square = position.pieceSquare(color, i);
            const int type = typeOf(position.pieceAt(square));
            if (type == PAWN) scores[color] += passedPawn(position, pawns, square, color);
            if (type >= KNIGHT && type <= QUEEN) {
                // Pawn-safe pseudo mobility avoids legal move generation and
                // make/unmake at every leaf. Pins are resolved by the search.
                const int moves = mobility(position, pawns, square, type, color);
                scores[color] += scaled(MOBILITY[type], moves - MOBILITY_BASE[type]);
            }
            if (type == ROOK && pawns.counts[color][fileOf(square)] == 0) {
                scores[color] += pawns.counts[color ^ 1][fileOf(square)] == 0
                    ? ROOK_OPEN_FILE : ROOK_SEMI_OPEN_FILE;
            }
        }
    }

    phase = std::min(phase, MAX_PHASE);
    const int mg = scores[WHITE].mg - scores[BLACK].mg;
    const int eg = scores[WHITE].eg - scores[BLACK].eg;
    // Blend the color difference once (rather than rounding each side) so
    // swapping colors and reflecting ranks preserves the exact score sign.
    int score = (mg * phase + eg * (MAX_PHASE - phase)) / MAX_PHASE;
    constexpr int MAX_STATIC_SCORE = VALUE_MATE_IN_MAX_PLY - 1;
    score = std::clamp(score, -MAX_STATIC_SCORE, MAX_STATIC_SCORE);
    return position.sideToMove() == WHITE ? score : -score;
}

} // namespace aether
