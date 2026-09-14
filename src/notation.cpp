#include "engine.hpp"

#include <algorithm>
#include <cctype>

namespace aether {
namespace {

std::string normalizeSan(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; }), text.end());
    std::replace(text.begin(), text.end(), '0', 'O');
    while (!text.empty() && (text.back() == '+' || text.back() == '#' || text.back() == '!' || text.back() == '?')) text.pop_back();
    return text;
}

char sanPiece(int type) {
    static constexpr char chars[] = "?PNBRQK";
    return type >= PAWN && type <= KING ? chars[type] : '?';
}

} // namespace

std::uint64_t perft(Position& position, int depth) {
    if (depth <= 0) return 1;
    MoveList moves;
    position.generate(moves);
    std::uint64_t nodes = 0;
    for (const Move& move : moves) {
        Undo undo;
        if (!position.makeMove(move, undo)) continue;
        nodes += perft(position, depth - 1);
        position.unmakeMove(move, undo);
    }
    return nodes;
}

std::vector<std::pair<std::string, std::uint64_t>> perftDivide(Position& position, int depth) {
    std::vector<std::pair<std::string, std::uint64_t>> result;
    MoveList moves;
    position.generate(moves);
    for (const Move& move : moves) {
        Undo undo;
        if (!position.makeMove(move, undo)) continue;
        const std::uint64_t nodes = depth <= 1 ? 1 : perft(position, depth - 1);
        position.unmakeMove(move, undo);
        result.emplace_back(move.uci(), nodes);
    }
    return result;
}

Move parseMove(const Position& position, const std::string& text) {
    const std::vector<Move> moves = position.legalMoves();
    for (const Move& move : moves) if (move.uci() == text) return move;
    const std::string wanted = normalizeSan(text);
    for (const Move& move : moves) if (normalizeSan(toSan(position, move)) == wanted) return move;
    return Move{};
}

std::string toSan(const Position& position, const Move& move) {
    if (move.isNull()) return "--";
    const int type = typeOf(position.pieceAt(move.from));
    std::string result;
    if (move.isCastle()) result = move.flag == KING_CASTLE ? "O-O" : "O-O-O";
    else {
        if (type != PAWN) {
            result += sanPiece(type);
            bool ambiguous = false;
            bool sameFile = false;
            bool sameRank = false;
            for (const Move& candidate : position.legalMoves()) {
                if (candidate.from == move.from || candidate.to != move.to || typeOf(position.pieceAt(candidate.from)) != type) continue;
                ambiguous = true;
                sameFile = sameFile || fileOf(candidate.from) == fileOf(move.from);
                sameRank = sameRank || rankOf(candidate.from) == rankOf(move.from);
            }
            if (ambiguous) {
                if (!sameFile) result += static_cast<char>('a' + fileOf(move.from));
                else if (!sameRank) result += static_cast<char>('1' + rankOf(move.from));
                else result += squareName(move.from);
            }
        } else if (move.isCapture()) result += static_cast<char>('a' + fileOf(move.from));
        if (move.isCapture()) result += 'x';
        result += squareName(move.to);
        if (move.isPromotion()) {
            result += '=';
            result += sanPiece(move.promotionType());
        }
    }
    Position copy = position;
    Undo undo;
    if (copy.makeMove(move, undo) && copy.inCheck()) result += copy.hasLegalMove() ? '+' : '#';
    return result;
}

std::string describeResult(GameResult result) {
    switch (result) {
        case GameResult::Ongoing: return "ongoing";
        case GameResult::WhiteWins: return "1-0";
        case GameResult::BlackWins: return "0-1";
        case GameResult::DrawStalemate: return "1/2-1/2 (stalemate)";
        case GameResult::DrawFiftyMove: return "1/2-1/2 (fifty-move rule)";
        case GameResult::DrawRepetition: return "1/2-1/2 (repetition)";
        case GameResult::DrawMaterial: return "1/2-1/2 (insufficient material)";
    }
    return "unknown";
}

} // namespace aether
