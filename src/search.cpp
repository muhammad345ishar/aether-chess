#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aether {
namespace {

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int scoreToTt(int score, int ply) {
    if (score >= VALUE_MATE_IN_MAX_PLY) return score + ply;
    if (score <= -VALUE_MATE_IN_MAX_PLY) return score - ply;
    return score;
}

int scoreFromTt(int score, int ply) {
    if (score >= VALUE_MATE_IN_MAX_PLY) return score - ply;
    if (score <= -VALUE_MATE_IN_MAX_PLY) return score + ply;
    return score;
}

bool isDraw(const Position& position, int ply) {
    return position.halfmoveClock() >= 100 || position.insufficientMaterial()
        || position.isRepetition(ply == 0 ? 2 : 1);
}

} // namespace

// Only score a stage when it is reached, and select its next move on demand.
// Most cut nodes therefore never score their quiet moves or sort a full list.
class Searcher::MovePicker {
public:
    MovePicker(Searcher& searcher, const Position& position, const Move& ttMove, int ply, bool tacticalOnly = false)
        : searcher_(searcher), position_(position), ttMove_(ttMove), ply_(ply) {
        position.generate(moves_, tacticalOnly);
    }

    Move next() {
        for (;;) {
            switch (stage_) {
            case Stage::Transposition: {
                stage_ = Stage::ScoreTactical;
                const Move move = takeNamed(ttMove_);
                if (!move.isNull()) return move;
                break;
            }
            case Stage::ScoreTactical:
                scoreTactical();
                stage_ = Stage::GoodTactical;
                break;
            case Stage::GoodTactical: {
                const Move move = takeBest(true, true);
                if (!move.isNull()) return move;
                stage_ = Stage::FirstKiller;
                break;
            }
            case Stage::FirstKiller:
            case Stage::SecondKiller: {
                const int index = stage_ == Stage::FirstKiller ? 0 : 1;
                const Move killer = searcher_.killers_[static_cast<std::size_t>(ply_)][static_cast<std::size_t>(index)];
                stage_ = index == 0 ? Stage::SecondKiller : Stage::ScoreQuiet;
                const Move move = takeNamed(killer);
                if (!move.isNull()) return move;
                break;
            }
            case Stage::ScoreQuiet:
                for (int i = 0; i < moves_.size(); ++i) {
                    const Move move = moves_[i];
                    if (!isTactical(move)) scores_[static_cast<std::size_t>(i)] =
                        searcher_.history_[static_cast<std::size_t>(position_.sideToMove())][move.from][move.to];
                }
                stage_ = Stage::Quiet;
                break;
            case Stage::Quiet: {
                const Move move = takeBest(false, false);
                if (!move.isNull()) return move;
                stage_ = Stage::BadTactical;
                break;
            }
            case Stage::BadTactical:
                return takeBest(true, false);
            default:
                return Move{};
            }
        }
    }

private:
    enum class Stage { Transposition, ScoreTactical, GoodTactical, FirstKiller, SecondKiller, ScoreQuiet, Quiet, BadTactical };

    Searcher& searcher_;
    const Position& position_;
    Move ttMove_{};
    int ply_ = 0;
    Stage stage_ = Stage::Transposition;
    MoveList moves_;
    std::array<int, 256> scores_{};
    std::array<bool, 256> good_{};

    static bool isTactical(const Move& move) { return move.isCapture() || move.isPromotion(); }

    Move take(int index) {
        const Move move = moves_[index];
        const int last = --moves_.count;
        moves_[index] = moves_[last];
        scores_[static_cast<std::size_t>(index)] = scores_[static_cast<std::size_t>(last)];
        good_[static_cast<std::size_t>(index)] = good_[static_cast<std::size_t>(last)];
        return move;
    }

    Move takeNamed(const Move& wanted) {
        if (!wanted.isNull()) {
            for (int i = 0; i < moves_.size(); ++i) if (moves_[i] == wanted) return take(i);
        }
        return Move{};
    }

    void scoreTactical() {
        for (int i = 0; i < moves_.size(); ++i) {
            const Move move = moves_[i];
            if (!isTactical(move)) continue;
            const int victim = move.flag == EP_CAPTURE ? PAWN : typeOf(position_.pieceAt(move.to));
            const int see = position_.see(move);
            scores_[static_cast<std::size_t>(i)] = 16 * pieceValue(victim)
                - pieceValue(typeOf(position_.pieceAt(move.from))) + see
                + (move.isPromotion() ? 16 * pieceValue(move.promotionType()) : 0);
            // SEE affects order only: checks, pins and longer combinations can
            // make an apparently losing exchange the only correct move.
            good_[static_cast<std::size_t>(i)] = see >= 0 || move.isPromotion();
        }
    }

    Move takeBest(bool tactical, bool good) {
        int best = -1;
        for (int i = 0; i < moves_.size(); ++i) {
            if (isTactical(moves_[i]) != tactical) continue;
            if (tactical && good_[static_cast<std::size_t>(i)] != good) continue;
            if (best < 0 || scores_[static_cast<std::size_t>(i)] > scores_[static_cast<std::size_t>(best)]) best = i;
        }
        return best < 0 ? Move{} : take(best);
    }
};

Searcher::Searcher() { setHashSizeMb(32); }

void Searcher::setHashSizeMb(int mb) {
    const std::size_t bytes = static_cast<std::size_t>(std::clamp(mb, 1, 1024)) * 1024 * 1024;
    tt_.assign(std::max<std::size_t>(1, bytes / sizeof(TTEntry)), TTEntry{});
    used_ = 0;
}

void Searcher::clearHash() {
    std::fill(tt_.begin(), tt_.end(), TTEntry{});
    used_ = 0;
}

void Searcher::newGame() {
    clearHash();
    history_ = {};
    killers_ = {};
    age_ = 0;
}

int Searcher::hashfullPermille() const {
    if (tt_.empty()) return 0;
    return static_cast<int>(std::min<std::size_t>(1000, used_ * 1000 / tt_.size()));
}

Searcher::TTEntry* Searcher::probe(std::uint64_t key) {
    if (tt_.empty()) return nullptr;
    TTEntry& entry = tt_[static_cast<std::size_t>(key % tt_.size())];
    return entry.key == key && entry.bound != BOUND_NONE ? &entry : nullptr;
}

void Searcher::store(const Position& position, int depth, int score, int bound, const Move& best, int ply) {
    if (tt_.empty() || aborted_) return;
    const std::uint64_t key = position.key();
    TTEntry& entry = tt_[static_cast<std::size_t>(key % tt_.size())];
    if (entry.bound == BOUND_NONE) ++used_;
    if (entry.key != key || entry.age != age_ || depth >= entry.depth || bound == BOUND_EXACT
        || entry.halfmoveClock != position.halfmoveClock() || entry.repetitionContext != position.repetitionContext()) {
        entry.key = key;
        entry.halfmoveClock = static_cast<std::uint16_t>(position.halfmoveClock());
        entry.repetitionContext = position.repetitionContext();
        entry.score = scoreToTt(score, ply);
        entry.best = best;
        entry.depth = static_cast<std::int16_t>(depth);
        entry.bound = static_cast<std::uint8_t>(bound);
        entry.age = age_;
    }
}

void Searcher::setUpTiming(const Position& position, const SearchLimits& limits) {
    softLimitMs_ = 0;
    hardLimitMs_ = 0;
    if (limits.infinite) return;
    if (limits.moveTimeMs > 0) {
        hardLimitMs_ = std::max(1, limits.moveTimeMs - 5);
        softLimitMs_ = hardLimitMs_;
        return;
    }
    const int color = position.sideToMove();
    const int remaining = limits.timeLeftMs[color];
    if (remaining < 0) return;
    const int moves = limits.movesToGo > 0 ? limits.movesToGo : 30;
    const std::int64_t allocation = static_cast<std::int64_t>(remaining) / moves
        + static_cast<std::int64_t>(std::max(0, limits.incrementMs[color])) * 3 / 4;
    softLimitMs_ = static_cast<int>(std::clamp<std::int64_t>(allocation, 1, std::max(1, remaining - 20)));
    hardLimitMs_ = static_cast<int>(std::clamp<std::int64_t>(allocation * 4, softLimitMs_, std::max(1, remaining - 5)));
}

bool Searcher::timeUp(bool checkClock) {
    if (stop_.load(std::memory_order_relaxed)) { aborted_ = true; return true; }
    if (nodeLimit_ != 0 && nodes_ >= nodeLimit_) { aborted_ = true; return true; }
    if (checkClock && hardLimitMs_ > 0 && nowMs() - startMs_ >= static_cast<std::uint64_t>(hardLimitMs_)) { aborted_ = true; return true; }
    return false;
}

bool Searcher::enterNode(int ply) {
    // Cheap cancellation and exact node-budget checks run on every node;
    // querying the monotonic clock remains amortized across 1024 nodes.
    if (timeUp((nodes_ & 1023ULL) == 0)) return false;
    ++nodes_;
    selDepth_ = std::max(selDepth_, ply);
    pvLength_[static_cast<std::size_t>(ply)] = ply;
    return true;
}

void Searcher::updatePv(int ply, const Move& move) {
    pvTable_[static_cast<std::size_t>(ply)][static_cast<std::size_t>(ply)] = move;
    const int childLength = pvLength_[static_cast<std::size_t>(ply + 1)];
    for (int i = ply + 1; i < childLength; ++i) {
        pvTable_[static_cast<std::size_t>(ply)][static_cast<std::size_t>(i)] = pvTable_[static_cast<std::size_t>(ply + 1)][static_cast<std::size_t>(i)];
    }
    pvLength_[static_cast<std::size_t>(ply)] = std::max(ply + 1, childLength);
}

int Searcher::quiescence(Position& position, int alpha, int beta, int ply) {
    if (!enterNode(ply)) return 0;
    const bool check = position.inCheck();
    if (isDraw(position, ply)) {
        // A mating move ends the game before a fifty-move claim can apply.
        if (check && !position.hasLegalMove()) return -VALUE_MATE + ply;
        return 0;
    }
    // Stand pat assumes the side may play a legal move. In stalemate that
    // assumption is false, including when stand pat would fail high.
    if ((!check || ply >= MAX_PLY) && !position.hasLegalMove()) return check ? -VALUE_MATE + ply : 0;
    if (ply >= MAX_PLY) return evaluate(position);
    if (!check) {
        const int standPat = evaluate(position);
        if (standPat >= beta) return standPat;
        alpha = std::max(alpha, standPat);
    }

    MovePicker moves(*this, position, Move{}, ply, !check);
    int legal = 0;
    for (Move move = moves.next(); !move.isNull(); move = moves.next()) {
        Undo undo;
        if (!position.makeMove(move, undo)) continue;
        ++legal;
        const int score = -quiescence(position, -beta, -alpha, ply + 1);
        position.unmakeMove(move, undo);
        if (aborted_) return 0;
        if (score >= beta) return score;
        if (score > alpha) { alpha = score; updatePv(ply, move); }
    }
    if (check && legal == 0) return -VALUE_MATE + ply;
    return alpha;
}

int Searcher::negamax(Position& position, int depth, int alpha, int beta, int ply, bool allowNull) {
    // The horizon belongs to quiescence and is counted once, not once in
    // each search routine. Quiescence itself searches all check evasions.
    if (depth <= 0 || ply >= MAX_PLY) return quiescence(position, alpha, beta, ply);
    if (!enterNode(ply)) return 0;
    const bool check = position.inCheck();
    if (isDraw(position, ply)) {
        if (check && !position.hasLegalMove()) return -VALUE_MATE + ply;
        return 0;
    }
    if (check) ++depth;
    const int originalAlpha = alpha;
    Move ttMove{};
    if (TTEntry* entry = probe(position.key())) {
        ttMove = entry->best;
        // The board key deliberately excludes the draw clock and history:
        // it is also used for repetition. A different context may suggest a
        // move, but cannot prove a score bound in the current position.
        if (entry->depth >= depth && ply > 0 && entry->halfmoveClock == position.halfmoveClock()
            && entry->repetitionContext == position.repetitionContext()) {
            const int ttScore = scoreFromTt(entry->score, ply);
            if (entry->bound == BOUND_EXACT) return ttScore;
            if (entry->bound == BOUND_LOWER && ttScore >= beta) return ttScore;
            if (entry->bound == BOUND_UPPER && ttScore <= alpha) return ttScore;
        }
    }

    const int staticEval = evaluate(position);
    evalStack_[static_cast<std::size_t>(ply)] = staticEval;
    if (allowNull && !check && depth >= 3 && ply > 0 && staticEval >= beta
        && position.halfmoveClock() + depth < 100 && position.hasNonPawnMaterial(position.sideToMove())) {
        // Passing cannot justify a cutoff in a position with no legal move.
        if (!position.hasLegalMove()) return 0;
        Undo undo;
        position.makeNullMove(undo);
        const int reduction = 2 + depth / 4;
        const int score = -negamax(position, depth - 1 - reduction, -beta, -beta + 1, ply + 1, false);
        position.unmakeNullMove(undo);
        if (aborted_) return 0;
        if (score >= beta && score < VALUE_MATE_IN_MAX_PLY) return score;
    }

    MovePicker moves(*this, position, ttMove, ply);
    Move bestMove{};
    int bestScore = -VALUE_INF;
    int legal = 0;
    for (Move move = moves.next(); !move.isNull(); move = moves.next()) {
        Undo undo;
        if (!position.makeMove(move, undo)) continue;
        ++legal;
        int score;
        if (legal == 1) score = -negamax(position, depth - 1, -beta, -alpha, ply + 1, true);
        else {
            int reduction = 0;
            if (depth >= 3 && legal > 3 && !check && !move.isCapture() && !move.isPromotion()
                && !position.inCheck()) reduction = 1 + (legal > 10 && depth >= 6 ? 1 : 0);
            score = -negamax(position, depth - 1 - reduction, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && reduction > 0) score = -negamax(position, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && score < beta) score = -negamax(position, depth - 1, -beta, -alpha, ply + 1, true);
        }
        position.unmakeMove(move, undo);
        if (aborted_) return 0;
        if (score > bestScore) { bestScore = score; bestMove = move; }
        if (score > alpha) { alpha = score; updatePv(ply, move); }
        if (alpha >= beta) {
            if (!move.isCapture() && !move.isPromotion()) {
                killers_[static_cast<std::size_t>(ply)][1] = killers_[static_cast<std::size_t>(ply)][0];
                killers_[static_cast<std::size_t>(ply)][0] = move;
                int& value = history_[static_cast<std::size_t>(position.sideToMove())][move.from][move.to];
                value = std::min(200'000, value + depth * depth);
            }
            break;
        }
    }
    if (legal == 0) return check ? -VALUE_MATE + ply : 0;
    const int bound = bestScore <= originalAlpha ? BOUND_UPPER : bestScore >= beta ? BOUND_LOWER : BOUND_EXACT;
    store(position, depth, bestScore, bound, bestMove, ply);
    return bestScore;
}

SearchReport Searcher::search(Position& position, const SearchLimits& limits) {
    // Reset only when leaving, never on worker entry: stop() can arrive
    // between prepareSearch() and the worker actually starting execution.
    struct ResetStopOnExit {
        std::atomic<bool>& stop;
        ~ResetStopOnExit() { stop.store(false, std::memory_order_relaxed); }
    } resetStop{stop_};
    aborted_ = false;
    nodes_ = 0;
    nodeLimit_ = limits.nodes;
    startMs_ = nowMs();
    selDepth_ = 0;
    rootColor_ = position.sideToMove();
    ++age_;
    pvLength_.fill(0);
    setUpTiming(position, limits);

    SearchReport completed;
    const std::vector<Move> rootMoves = position.legalMoves();
    if (rootMoves.empty()) {
        completed.score = position.inCheck() ? -VALUE_MATE : 0;
        completed.mate = position.inCheck();
        completed.elapsedMs = nowMs() - startMs_;
        if (onInfo_) onInfo_(completed);
        return completed;
    }
    completed.best = rootMoves.front();
    if (isDraw(position, 0)) {
        completed.elapsedMs = nowMs() - startMs_;
        if (onInfo_) onInfo_(completed);
        return completed;
    }
    const int maximumDepth = std::clamp(limits.depth, 1, MAX_PLY - 8);
    std::uint64_t lastReportedNodes = 0;
    for (int depth = 1; depth <= maximumDepth; ++depth) {
        selDepth_ = 0;
        pvLength_.fill(0);
        const int score = negamax(position, depth, -VALUE_INF, VALUE_INF, 0, true);
        if (aborted_) break;
        completed.depth = depth;
        completed.selDepth = selDepth_;
        completed.score = score;
        completed.nodes = nodes_;
        completed.elapsedMs = nowMs() - startMs_;
        completed.pv.clear();
        completed.ponder = Move{};
        for (int i = 0; i < pvLength_[0]; ++i) completed.pv.push_back(pvTable_[0][static_cast<std::size_t>(i)]);
        if (!completed.pv.empty()) completed.best = completed.pv.front();
        if (completed.pv.size() > 1) completed.ponder = completed.pv[1];
        completed.mate = std::abs(score) >= VALUE_MATE_IN_MAX_PLY;
        completed.mateIn = 0;
        if (completed.mate) {
            const int plies = VALUE_MATE - std::abs(score);
            completed.mateIn = (score > 0 ? 1 : -1) * ((plies + 1) / 2);
        }
        if (onInfo_) onInfo_(completed);
        lastReportedNodes = nodes_;
        if (completed.mate || (softLimitMs_ > 0 && completed.elapsedMs >= static_cast<std::uint64_t>(softLimitMs_))) break;
        if (timeUp()) break;
    }
    completed.nodes = nodes_;
    completed.elapsedMs = nowMs() - startMs_;
    // An interrupted iteration retains the last completed PV and depth, but
    // consumers still need the actual nodes/time spent before cancellation.
    if (onInfo_ && (aborted_ || completed.nodes != lastReportedNodes)) onInfo_(completed);
    return completed;
}

} // namespace aether
