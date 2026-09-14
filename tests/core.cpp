#include "engine.hpp"

#include <iostream>
#include <string>

int main() {
    using namespace aether;

    Position position;
    const std::string original = position.fen();
    const char* moves[] = {"e4", "e5", "Nf3", "Nc6", "Bb5", "a6", "Ba4", "Nf6", "O-O"};
    for (const char* text : moves) {
        const Move move = parseMove(position, text);
        if (move.isNull()) {
            std::cerr << "could not parse " << text << '\n';
            return 1;
        }
        Undo undo;
        if (!position.makeMove(move, undo) || !position.consistent()) {
            std::cerr << "failed to play " << text << '\n';
            return 1;
        }
    }

    Position mate("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");
    if (mate.result() != GameResult::WhiteWins) {
        std::cerr << "checkmate detection failed\n";
        return 1;
    }
    Position stalemate("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    if (stalemate.result() != GameResult::DrawStalemate) {
        std::cerr << "stalemate detection failed\n";
        return 1;
    }
    Position invalid;
    if (invalid.setFen("8/8/8/8/8/8/8/8 w - - 0 1") || invalid.fen() != original) {
        std::cerr << "invalid FEN handling failed\n";
        return 1;
    }

    Searcher searcher;
    SearchLimits limits;
    limits.depth = 4;
    Position start;
    const SearchReport report = searcher.search(start, limits);
    if (report.best.isNull() || parseMove(start, report.best.uci()).isNull() || report.depth != 4) {
        std::cerr << "search failed to return a legal depth-4 move\n";
        return 1;
    }

    std::cout << "core ok\n";
    return 0;
}
