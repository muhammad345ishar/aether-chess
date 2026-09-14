#include "engine.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

using namespace aether;

namespace {

template <typename T>
bool readNumber(std::istringstream& input, T& value, T minimum, T maximum) {
    std::string text;
    if (!(input >> text)) return false;
    T parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || parsed < minimum || parsed > maximum) return false;
    value = parsed;
    return true;
}

bool readPosition(std::istringstream& input, Position& position) {
    // Parse into a temporary board so a rejected FEN or move cannot leave the
    // GUI and engine looking at different partial positions.
    Position next;
    std::string token;
    if (!(input >> token)) return false;
    if (token == "fen") {
        std::string fen, field;
        for (int i = 0; i < 6; ++i) {
            if (!(input >> field)) return false;
            if (i) fen += ' ';
            fen += field;
        }
        if (!next.setFen(fen)) return false;
    } else if (token != "startpos") return false;
    if (input >> token) {
        if (token != "moves") return false;
        while (input >> token) {
            const Move move = parseMove(next, token);
            Undo undo;
            if (move.isNull() || !next.makeMove(move, undo)) return false;
        }
    }
    position = std::move(next);
    return true;
}

bool readLimits(std::istringstream& input, SearchLimits& limits) {
    std::string token;
    constexpr int maximum = std::numeric_limits<int>::max();
    while (input >> token) {
        if (token == "infinite") limits.infinite = true;
        else if (token == "depth") {
            if (!readNumber(input, limits.depth, 1, MAX_PLY - 8)) return false;
        } else if (token == "nodes") {
            if (!readNumber(input, limits.nodes, std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max())) return false;
        } else if (token == "movetime") {
            if (!readNumber(input, limits.moveTimeMs, 0, maximum)) return false;
            limits.moveTimeMs = std::max(1, limits.moveTimeMs);
        } else if (token == "wtime" || token == "btime") {
            if (!readNumber(input, limits.timeLeftMs[token == "wtime" ? WHITE : BLACK], 0, maximum)) return false;
        } else if (token == "winc" || token == "binc") {
            if (!readNumber(input, limits.incrementMs[token == "winc" ? WHITE : BLACK], 0, maximum)) return false;
        } else if (token == "movestogo") {
            if (!readNumber(input, limits.movesToGo, 1, maximum)) return false;
        } else {
            // Unsupported restrictions must not become unrestricted searches.
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    Position position;
    Searcher searcher;
    std::mutex outputMutex;
    std::mutex lifecycleMutex;
    std::condition_variable stopped;
    bool stopRequested = false;
    std::thread searchThread;

    auto message = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << text << '\n' << std::flush;
    };
    auto stopSearch = [&]() {
        if (!searchThread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            stopRequested = true;
            searcher.stop();
        }
        stopped.notify_all();
        searchThread.join();
    };
    searcher.setInfoHandler([&](const SearchReport& report) {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << "info depth " << report.depth << " seldepth " << report.selDepth;
        if (report.mate) std::cout << " score mate " << report.mateIn;
        else std::cout << " score cp " << report.score;
        std::cout << " nodes " << report.nodes << " time " << report.elapsedMs;
        if (report.elapsedMs > 0) std::cout << " nps " << report.nodes * 1000 / report.elapsedMs;
        std::cout << " hashfull " << searcher.hashfullPermille();
        if (!report.pv.empty()) {
            std::cout << " pv";
            for (const Move& move : report.pv) std::cout << ' ' << move.uci();
        }
        std::cout << '\n' << std::flush;
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command == "uci") {
            message("id name Aether 0.2\nid author Muhammad Ishar\n"
                    "option name Hash type spin default 32 min 1 max 1024\n"
                    "option name Clear Hash type button\nuciok");
        } else if (command == "isready") {
            message("readyok");
        } else if (command == "ucinewgame") {
            stopSearch();
            (void)position.setFen(startFen());
            searcher.newGame();
        } else if (command == "setoption") {
            stopSearch();
            std::string token, name;
            if (!(input >> token) || token != "name") {
                message("info string invalid setoption command");
                continue;
            }
            while (input >> token && token != "value") {
                if (!name.empty()) name += ' ';
                name += token;
            }
            if (name == "Hash") {
                int mb = 0;
                if (token != "value" || !readNumber(input, mb, 1, 1024)) message("info string invalid Hash size");
                else searcher.setHashSizeMb(mb);
            } else if (name == "Clear Hash") searcher.clearHash();
        } else if (command == "position") {
            stopSearch();
            if (!readPosition(input, position)) message("info string invalid position command; previous position retained");
        } else if (command == "go") {
            stopSearch();
            SearchLimits limits;
            if (!readLimits(input, limits)) {
                message("info string invalid or unsupported go limits");
                continue;
            }
            // Reset before launching. A stop remains visible even if the
            // worker has not entered search() yet.
            searcher.prepareSearch();
            {
                std::lock_guard<std::mutex> lock(lifecycleMutex);
                stopRequested = false;
            }
            searchThread = std::thread([&, limits]() {
                const SearchReport report = searcher.search(position, limits);
                if (limits.infinite) {
                    // Even a terminal position must wait for UCI stop before
                    // returning bestmove during infinite analysis.
                    std::unique_lock<std::mutex> lock(lifecycleMutex);
                    stopped.wait(lock, [&]() { return stopRequested; });
                }
                std::lock_guard<std::mutex> lock(outputMutex);
                std::cout << "bestmove " << report.best.uci();
                if (!report.ponder.isNull()) std::cout << " ponder " << report.ponder.uci();
                std::cout << '\n' << std::flush;
            });
        } else if (command == "stop") stopSearch();
        else if (command == "perft") {
            stopSearch();
            int depth = 0;
            if (!readNumber(input, depth, 0, MAX_PLY - 8)) message("info string invalid perft depth");
            else message(std::to_string(perft(position, depth)));
        } else if (command == "quit") {
            stopSearch();
            break;
        }
    }
    stopSearch();
    return 0;
}
