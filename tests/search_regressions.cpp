#include "engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aether {

// Keep horizon and TT regressions precise without making search internals a
// production API or changing access keywords with preprocessor macros.
struct SearcherTestAccess {
    static void reset(Searcher& searcher, const Position& root) {
        searcher.prepareSearch();
        searcher.aborted_ = false;
        searcher.nodes_ = 0;
        searcher.nodeLimit_ = 0;
        searcher.hardLimitMs_ = 0;
        searcher.softLimitMs_ = 0;
        searcher.selDepth_ = 0;
        searcher.rootGamePly_ = root.gamePly();
        searcher.rootHistoryContext_ = root.repetitionContextBeforeCurrent();
        searcher.pvLength_.fill(0);
    }

    static int quiescence(Searcher& searcher, Position& position, int alpha = -VALUE_INF, int beta = VALUE_INF,
        int ply = 0, const Position* root = nullptr) {
        reset(searcher, root ? *root : position);
        return searcher.quiescence(position, alpha, beta, ply);
    }

    static int cachedSearch(Searcher& searcher, Position& position, const Position* root = nullptr) {
        reset(searcher, root ? *root : position);
        return searcher.negamax(position, 1, -VALUE_INF, VALUE_INF, 1, false);
    }

    static void seedExact(Searcher& searcher, const Position& position, int score, const Position* root = nullptr) {
        reset(searcher, root ? *root : position);
        searcher.store(position, 5, score, Searcher::BOUND_EXACT, Move{}, 1);
    }

    static std::pair<int, int> timing(Searcher& searcher, const Position& position, const SearchLimits& limits) {
        searcher.setUpTiming(position, limits);
        return {searcher.softLimitMs_, searcher.hardLimitMs_};
    }
};

} // namespace aether

namespace {

using namespace aether;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "search regression: " << message << '\n';
        std::exit(1);
    }
}

struct Snapshot {
    std::string fen;
    std::uint64_t key;
    std::uint64_t context;
    int gamePly;

    explicit Snapshot(const Position& position)
        : fen(position.fen()), key(position.key()), context(position.repetitionContext()), gamePly(position.gamePly()) {}

    void check(const Position& position, const std::string& label) const {
        require(position.fen() == fen && position.key() == key && position.repetitionContext() == context
            && position.gamePly() == gamePly && position.consistent(), label + " changed the position");
    }
};

void play(Position& position, const char* text) {
    const Move move = parseMove(position, text);
    Undo undo;
    require(!move.isNull() && position.makeMove(move, undo), std::string("illegal fixture move ") + text);
}

bool legalBest(const Position& position, const SearchReport& report) {
    return !report.best.isNull() && parseMove(position, report.best.uci()) == report.best;
}

void validatePv(const Position& root, const SearchReport& report) {
    Position position = root;
    for (const Move& move : report.pv) {
        require(parseMove(position, move.uci()) == move, "PV contains an illegal move");
        Undo undo;
        require(position.makeMove(move, undo), "PV move could not be played");
    }
    require(report.ponder == (report.pv.size() > 1 ? report.pv[1] : Move{}), "stale ponder after PV shortened");
    require(report.pv.empty() || report.best == report.pv.front(), "best move differs from PV");
}

void mateAndDraws() {
    Searcher searcher;
    SearchLimits limits;
    limits.depth = 5;

    Position mateInOne("7k/5Q2/5K2/8/8/8/8/8 w - - 99 1");
    const Snapshot before(mateInOne);
    const SearchReport mate = searcher.search(mateInOne, limits);
    require(mate.best.uci() == "f7g7" && mate.mate && mate.mateIn == 1 && mate.score == VALUE_MATE - 1,
        "mate on halfmove 100 was treated as a draw");
    before.check(mateInOne, "mate search");
    validatePv(mateInOne, mate);

    Position checkmate("7k/6Q1/5K2/8/8/8/8/8 b - - 100 1");
    const SearchReport terminalMate = searcher.search(checkmate, limits);
    require(terminalMate.best.isNull() && terminalMate.mate && terminalMate.score == -VALUE_MATE,
        "terminal root mate did not take precedence over draw clock");

    Position stalemate("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    const SearchReport terminalDraw = searcher.search(stalemate, limits);
    require(terminalDraw.best.isNull() && !terminalDraw.mate && terminalDraw.score == 0, "root stalemate score");

    Position fifty("7k/8/5K2/8/8/8/8/Q7 w - - 100 1");
    const SearchReport fiftyDraw = searcher.search(fifty, limits);
    require(fiftyDraw.score == 0 && !fiftyDraw.mate && legalBest(fifty, fiftyDraw), "root fifty-move legal fallback");

    Position material("7k/8/5K2/8/8/8/8/8 w - - 0 1");
    const SearchReport materialDraw = searcher.search(material, limits);
    require(materialDraw.score == 0 && legalBest(material, materialDraw), "root insufficient-material legal fallback");

    Position repetition("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (const char* move : {"g1f3", "g8f6", "f3g1", "f6g8"}) play(repetition, move);
    }
    require(repetition.isRepetition(), "threefold fixture");
    const Snapshot repeatedBefore(repetition);
    const SearchReport repetitionDraw = searcher.search(repetition, limits);
    require(repetitionDraw.score == 0 && legalBest(repetition, repetitionDraw), "root repetition legal fallback");
    repeatedBefore.check(repetition, "root repetition search");
}

void transpositionContexts() {
    SearchLimits limits;
    limits.depth = 5;
    Searcher warm;
    Searcher cold;
    Position lowClock("7k/8/5K2/8/8/8/8/Q7 w - - 0 1");
    Position nearDraw("7k/8/5K2/8/8/8/8/Q7 w - - 98 1");
    require(lowClock.key() == nearDraw.key(), "draw clock polluted repetition key");
    const Snapshot before(nearDraw);
    const SearchReport winning = warm.search(lowClock, limits);
    require(winning.score > 0, "low-clock winning fixture");
    const SearchReport warmReport = warm.search(nearDraw, limits);
    const SearchReport coldReport = cold.search(nearDraw, limits);
    require(warmReport.score == 0 && coldReport.score == 0 && !warmReport.mate && !coldReport.mate,
        "warm TT reused a mating bound across different draw clocks");
    before.check(nearDraw, "warm/cold searches");

    Position firstHistory;
    Position secondHistory;
    for (const char* move : {"g1f3", "g8f6", "b1c3", "b8c6"}) play(firstHistory, move);
    for (const char* move : {"b1c3", "b8c6", "g1f3", "g8f6"}) play(secondHistory, move);
    require(firstHistory.fen() == secondHistory.fen() && firstHistory.key() == secondHistory.key(), "TT history fixture boards differ");
    require(firstHistory.repetitionContext() != secondHistory.repetitionContext(), "different histories share a context");
    Searcher contextual;
    SearcherTestAccess::seedExact(contextual, firstHistory, 12345);
    require(SearcherTestAccess::cachedSearch(contextual, firstHistory) == 12345, "matching TT context did not cut off");
    const Snapshot historyBefore(secondHistory);
    require(SearcherTestAccess::cachedSearch(contextual, secondHistory) != 12345, "TT ignored repetition-history context");
    historyBefore.check(secondHistory, "history-sensitive TT search");

    // Even the identical history has different draw semantics when its current
    // node becomes the next search root. Do not reuse a bound from the old path.
    Position initial;
    Searcher rerooted;
    SearcherTestAccess::seedExact(rerooted, firstHistory, 12345, &initial);
    require(SearcherTestAccess::cachedSearch(rerooted, firstHistory, &initial) == 12345,
        "same search root/context did not reuse a bound");
    require(SearcherTestAccess::cachedSearch(rerooted, firstHistory) != 12345,
        "TT reused a bound after moving the root within identical history");

    // These histories have identical board/clock/total multiset and roots at
    // the same history index, but put different cycles before their roots.
    Position kingFirst, queenFirst;
    for (const char* move : {"g1f3", "g8f6", "f3g1", "f6g8"}) play(kingFirst, move);
    for (const char* move : {"b1c3", "b8c6", "c3b1", "c6b8"}) play(queenFirst, move);
    const Position kingRoot = kingFirst;
    const Position queenRoot = queenFirst;
    for (const char* move : {"b1c3", "b8c6", "c3b1", "c6b8", "b1a3"}) play(kingFirst, move);
    for (const char* move : {"g1f3", "g8f6", "f3g1", "f6g8", "b1a3"}) play(queenFirst, move);
    require(kingFirst.fen() == queenFirst.fen() && kingFirst.repetitionContext() == queenFirst.repetitionContext(),
        "split-history TT fixture has different total context");
    require(kingRoot.gamePly() == queenRoot.gamePly()
        && kingRoot.repetitionContextBeforeCurrent() != queenRoot.repetitionContextBeforeCurrent(),
        "split-history TT fixture has equivalent pre-root history");
    Searcher split;
    SearcherTestAccess::seedExact(split, kingFirst, 12345, &kingRoot);
    require(SearcherTestAccess::cachedSearch(split, kingFirst, &kingRoot) == 12345,
        "matching split-history context did not cut off");
    require(SearcherTestAccess::cachedSearch(split, queenFirst, &queenRoot) != 12345,
        "TT ignored how the root partitions an identical history multiset");
}

void searchRepetitions() {
    Position position("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const Position cycleRoot = position;
    for (const char* move : {"g1f3", "g8f6", "f3g1"}) play(position, move);
    const Position historicalRoot = position;
    const Snapshot before(position);
    SearchLimits limits;
    limits.depth = 4;
    Searcher searcher;
    const SearchReport losing = searcher.search(position, limits);
    require(losing.score < -500 && legalBest(position, losing),
        "second historical occurrence hid a missing queen as a draw");
    validatePv(position, losing);
    before.check(position, "historical second-occurrence search");

    play(position, "f6g8");
    require(position.isRepetition(1) && !position.isRepetition(), "second occurrence fixture");
    require(SearcherTestAccess::quiescence(searcher, position, -VALUE_INF, VALUE_INF, 1, &historicalRoot) > 500,
        "quiescence treated one pre-root occurrence as a draw");
    require(SearcherTestAccess::cachedSearch(searcher, position, &historicalRoot) > 500,
        "negamax treated one pre-root occurrence as a draw");
    require(SearcherTestAccess::quiescence(searcher, position, -VALUE_INF, VALUE_INF, 4, &cycleRoot) == 0,
        "quiescence missed a cycle back to the search root");
    require(SearcherTestAccess::cachedSearch(searcher, position, &cycleRoot) == 0,
        "negamax missed a cycle back to the search root");
    const SearchReport winning = searcher.search(position, limits);
    require(winning.score > 500, "second occurrence at the root was adjudicated a draw");

    for (const char* move : {"g1f3", "g8f6", "f3g1"}) play(position, move);
    const Snapshot thirdBefore(position);
    const SearchReport claim = searcher.search(position, limits);
    require(claim.score == 0 && claim.best.uci() == "f6g8",
        "search failed to choose a move producing a genuine third occurrence");
    thirdBefore.check(position, "third-occurrence search");
}

void horizonRegressions() {
    Searcher searcher;
    Position stalemate("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    const Snapshot staleBefore(stalemate);
    require(SearcherTestAccess::quiescence(searcher, stalemate) == 0, "quiescence missed stalemate");
    require(SearcherTestAccess::quiescence(searcher, stalemate, -2000, -1500) == 0,
        "stand-pat cutoff preceded stalemate detection");
    staleBefore.check(stalemate, "quiescence stalemate");

    Position fifty("7k/8/5K2/8/8/8/8/Q7 w - - 100 1");
    require(SearcherTestAccess::quiescence(searcher, fifty) == 0, "quiescence missed fifty-move draw");
    Position checkmate("7k/6Q1/5K2/8/8/8/8/8 b - - 100 1");
    require(SearcherTestAccess::quiescence(searcher, checkmate, -VALUE_INF, VALUE_INF, 3) == -VALUE_MATE + 3,
        "quiescence draw check hid mate");

    Position repetition("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const Position repetitionRoot = repetition;
    for (const char* move : {"g1f3", "g8f6", "f3g1", "f6g8"}) play(repetition, move);
    require(evaluate(repetition) > 500 && repetition.isRepetition(1), "horizon repetition fixture");
    require(SearcherTestAccess::quiescence(searcher, repetition, -VALUE_INF, VALUE_INF, 4, &repetitionRoot) == 0,
        "quiescence missed path repetition");

    Position promotion("7k/P7/6K1/8/8/8/8/8 w - - 0 1");
    const Snapshot promoBefore(promotion);
    MoveList tactical;
    promotion.generate(tactical, true);
    const int quietPromotions = static_cast<int>(std::count_if(tactical.begin(), tactical.end(), [](Move move) {
        return move.isPromotion() && !move.isCapture();
    }));
    require(quietPromotions == 4, "tactical generation omitted underpromotions");
    require(SearcherTestAccess::quiescence(searcher, promotion) == VALUE_MATE - 1,
        "quiescence omitted a quiet promotion mate");
    promoBefore.check(promotion, "quiescence quiet promotion");

    Position pinned("3k4/8/2pr4/1B6/8/8/8/3R2K1 w - - 0 1");
    const Move capture = parseMove(pinned, "b5c6");
    const Snapshot pinnedBefore(pinned);
    require(!capture.isNull() && pinned.see(capture) == pieceValue(PAWN), "SEE allowed an absolutely pinned recapture");
    (void)SearcherTestAccess::quiescence(searcher, pinned);
    pinnedBefore.check(pinned, "pinned-exchange quiescence");
}

void limitsAndReports() {
    Searcher searcher;
    Position position;
    const Snapshot before(position);
    SearchLimits limits;
    limits.depth = 8;
    limits.nodes = 1;
    std::vector<SearchReport> reports;
    searcher.setInfoHandler([&](const SearchReport& report) {
        reports.push_back(report);
        validatePv(position, report);
    });
    const SearchReport oneNode = searcher.search(position, limits);
    require(oneNode.nodes == 1 && oneNode.depth == 0 && legalBest(position, oneNode), "one-node limit overshot or lacked fallback");
    require(!reports.empty() && reports.back().nodes == oneNode.nodes, "interrupted search did not report total nodes");
    before.check(position, "one-node search");

    reports.clear();
    limits.nodes = 77;
    const SearchReport limited = searcher.search(position, limits);
    require(limited.nodes == 77 && legalBest(position, limited), "node budget was not enforced at node entry");
    require(!reports.empty() && reports.back().nodes == 77 && reports.back().depth == limited.depth,
        "last info omitted interrupted-iteration work");
    before.check(position, "limited search");

    searcher.prepareSearch();
    searcher.stop();
    const SearchReport stopped = searcher.search(position, limits);
    require(stopped.nodes == 0 && stopped.depth == 0 && legalBest(position, stopped), "search entry erased an incoming stop");
    before.check(position, "pre-stopped search");
    limits.nodes = 0;
    limits.depth = 3;
    const SearchReport reused = searcher.search(position, limits);
    require(reused.depth == 3 && reused.nodes > 0 && legalBest(position, reused), "stopped searcher could not be reused synchronously");
    before.check(position, "reused searcher");

    // A callback models stop arriving while an iteration is being published.
    searcher.setInfoHandler([&](const SearchReport& report) {
        validatePv(position, report);
        if (report.depth == 1) searcher.stop();
    });
    const SearchReport callbackStop = searcher.search(position, limits);
    require(callbackStop.depth == 1 && legalBest(position, callbackStop), "callback cancellation was ignored");
    before.check(position, "callback-stopped search");

    SearchLimits timing;
    require(SearcherTestAccess::timing(searcher, position, timing) == std::make_pair(0, 0), "absent clock got a deadline");
    timing.timeLeftMs[WHITE] = 0;
    require(SearcherTestAccess::timing(searcher, position, timing) == std::make_pair(1, 1), "zero clock was treated as unlimited");
    timing.timeLeftMs[WHITE] = std::numeric_limits<int>::max();
    timing.incrementMs[WHITE] = std::numeric_limits<int>::max();
    timing.movesToGo = 1;
    const auto large = SearcherTestAccess::timing(searcher, position, timing);
    require(large.first > 0 && large.second >= large.first && large.second <= std::numeric_limits<int>::max(),
        "clock allocation overflowed");
}

} // namespace

int main() {
    mateAndDraws();
    transpositionContexts();
    searchRepetitions();
    horizonRegressions();
    limitsAndReports();
    std::cout << "search regressions ok\n";
    return 0;
}
