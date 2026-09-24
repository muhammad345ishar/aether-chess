// Aether Chess — core engine interface.
//
// Board representation is 0x88: a 128-entry array where a square index encodes
// rank in bits 4-6 and file in bits 0-2. A square is off-board exactly when
// (sq & 0x88) != 0, which makes edge detection a single test and makes ray
// generation branch-cheap.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aether {

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------

enum Piece : int {
    EMPTY = 0,
    WP = 1, WN, WB, WR, WQ, WK,
    BP = 7, BN, BB, BR, BQ, BK
};

enum Color : int { WHITE = 0, BLACK = 1 };

enum PieceType : int { NO_TYPE = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

enum MoveFlag : std::uint8_t {
    QUIET = 0,
    DOUBLE_PUSH = 1,
    KING_CASTLE = 2,
    QUEEN_CASTLE = 3,
    CAPTURE = 4,
    EP_CAPTURE = 5,
    PROMOTE_N = 8,
    PROMOTE_B = 9,
    PROMOTE_R = 10,
    PROMOTE_Q = 11,
    PROMOTE_N_CAPTURE = 12,
    PROMOTE_B_CAPTURE = 13,
    PROMOTE_R_CAPTURE = 14,
    PROMOTE_Q_CAPTURE = 15
};

enum CastleRight : int { WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8 };

constexpr int MAX_PLY = 128;
constexpr int VALUE_INF = 32000;
constexpr int VALUE_NONE = 32001;
constexpr int VALUE_MATE = 31000;
constexpr int VALUE_MATE_IN_MAX_PLY = VALUE_MATE - MAX_PLY;

[[nodiscard]] constexpr bool onBoard(int sq) { return (sq & 0x88) == 0; }
[[nodiscard]] constexpr int rankOf(int sq) { return sq >> 4; }
[[nodiscard]] constexpr int fileOf(int sq) { return sq & 7; }
[[nodiscard]] constexpr int squareOf(int file, int rank) { return rank * 16 + file; }
[[nodiscard]] constexpr int colorOf(int piece) { return piece >= BP ? BLACK : WHITE; }
[[nodiscard]] constexpr int typeOf(int piece) { return piece >= BP ? piece - 6 : piece; }
[[nodiscard]] constexpr int makePiece(int color, int type) { return color == WHITE ? type : type + 6; }

[[nodiscard]] std::string squareName(int sq);
[[nodiscard]] int parseSquare(const std::string& name);

struct Move {
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    std::uint8_t flag = QUIET;

    constexpr Move() = default;
    constexpr Move(int f, int t, int fl = QUIET)
        : from(static_cast<std::uint8_t>(f)),
          to(static_cast<std::uint8_t>(t)),
          flag(static_cast<std::uint8_t>(fl)) {}

    [[nodiscard]] constexpr bool isNull() const { return from == 0 && to == 0; }
    [[nodiscard]] constexpr bool isCapture() const {
        return flag == CAPTURE || flag == EP_CAPTURE || flag >= PROMOTE_N_CAPTURE;
    }
    [[nodiscard]] constexpr bool isPromotion() const { return flag >= PROMOTE_N; }
    [[nodiscard]] constexpr bool isCastle() const { return flag == KING_CASTLE || flag == QUEEN_CASTLE; }
    // Promoted-to piece type (KNIGHT..QUEEN), or NO_TYPE for non-promotions.
    [[nodiscard]] constexpr int promotionType() const {
        return isPromotion() ? KNIGHT + ((flag - PROMOTE_N) & 3) : NO_TYPE;
    }
    [[nodiscard]] std::string uci() const;

    friend constexpr bool operator==(const Move& a, const Move& b) {
        return a.from == b.from && a.to == b.to && a.flag == b.flag;
    }
    friend constexpr bool operator!=(const Move& a, const Move& b) { return !(a == b); }
};

// Fixed-capacity move buffer. 256 is comfortably above the 218-move maximum
// reachable in a legal chess position, and avoids an allocation per node.
struct MoveList {
    std::array<Move, 256> data{};
    int count = 0;

    void add(Move move) {
        if (count < static_cast<int>(data.size())) data[static_cast<std::size_t>(count++)] = move;
    }
    void add(int from, int to, int flag = QUIET) { add(Move{from, to, flag}); }
    void clear() { count = 0; }
    [[nodiscard]] int size() const { return count; }
    [[nodiscard]] bool empty() const { return count == 0; }
    Move& operator[](int i) { return data[static_cast<std::size_t>(i)]; }
    const Move& operator[](int i) const { return data[static_cast<std::size_t>(i)]; }
    [[nodiscard]] Move* begin() { return data.data(); }
    [[nodiscard]] Move* end() { return data.data() + count; }
    [[nodiscard]] const Move* begin() const { return data.data(); }
    [[nodiscard]] const Move* end() const { return data.data() + count; }
};

// State that make/unmake cannot reconstruct from the move alone.
struct Undo {
    int moved = EMPTY;
    int captured = EMPTY;
    int captureSquare = -1;
    int movedIndex = -1;
    int capturedIndex = -1;
    int castling = 0;
    int epSquare = -1;
    int hashedEpFile = -1;
    int halfmove = 0;
    int fullmove = 1;
    std::uint64_t key = 0;
    std::size_t historySize = 0;
    std::size_t repetitionStart = 0;
    std::uint64_t repetitionContext = 0;
};

enum class GameResult { Ongoing, WhiteWins, BlackWins, DrawStalemate, DrawFiftyMove, DrawRepetition, DrawMaterial };

class Position {
public:
    Position();
    explicit Position(const std::string& fen);

    // Returns false and leaves the position untouched if the FEN is unusable.
    bool setFen(const std::string& fen);
    [[nodiscard]] std::string fen() const;

    [[nodiscard]] int sideToMove() const { return side_; }
    [[nodiscard]] int pieceAt(int sq) const { return board_[static_cast<std::size_t>(sq)]; }
    [[nodiscard]] std::uint64_t key() const { return key_; }
    // Reversible-position multiset, used to qualify history-dependent TT scores.
    [[nodiscard]] std::uint64_t repetitionContext() const { return repetitionContext_; }
    // Excludes the current occurrence, which belongs to the search path when
    // this position becomes a search root.
    [[nodiscard]] std::uint64_t repetitionContextBeforeCurrent() const;
    [[nodiscard]] int castlingRights() const { return castling_; }
    [[nodiscard]] int epSquare() const { return epSquare_; }
    [[nodiscard]] int halfmoveClock() const { return halfmove_; }
    [[nodiscard]] int fullmoveNumber() const { return fullmove_; }
    [[nodiscard]] int kingSquare(int color) const { return kingSq_[static_cast<std::size_t>(color)]; }
    [[nodiscard]] int pieceCount(int color) const { return count_[static_cast<std::size_t>(color)]; }
    [[nodiscard]] int pieceSquare(int color, int i) const { return list_[static_cast<std::size_t>(color)][static_cast<std::size_t>(i)]; }

    // capturesOnly selects tactical moves: captures AND all promotions.
    void generate(MoveList& out, bool capturesOnly = false) const;
    [[nodiscard]] std::vector<Move> legalMoves(bool capturesOnly = false) const;
    [[nodiscard]] bool hasLegalMove() const;
    // Reuse a complete pseudo-legal list generated for this exact position.
    // Legality only needs hypothetical occupancy, not a copy of game history.
    [[nodiscard]] bool hasLegalMove(const MoveList& candidates) const;

    [[nodiscard]] bool isSquareAttacked(int sq, int byColor) const;
    [[nodiscard]] bool inCheck() const { return isSquareAttacked(kingSq_[static_cast<std::size_t>(side_)], side_ ^ 1); }
    [[nodiscard]] bool inCheck(int color) const {
        return isSquareAttacked(kingSq_[static_cast<std::size_t>(color)], color ^ 1);
    }
    [[nodiscard]] bool givesCheck(const Move& move) const;

    // Applies `move`. Returns false when the move left the mover's own king in
    // check; in that case the position has already been restored and the caller
    // must NOT call unmakeMove. On true, the caller owns the unmake.
    [[nodiscard]] bool makeMove(const Move& move, Undo& undo);
    void unmakeMove(const Move& move, const Undo& undo);
    void makeNullMove(Undo& undo);
    void unmakeNullMove(const Undo& undo);

    // Static exchange estimate in centipawns, using least-value legal recaptures.
    // Pins and king safety are checked; off-target tactics are not searched.
    [[nodiscard]] int see(const Move& move) const;

    [[nodiscard]] bool isRepetition(int minCount = 2) const;
    // A cycle back to the search root (or a later path position) is a search
    // draw. Matches strictly before the root still require three occurrences.
    [[nodiscard]] bool isSearchRepetition(int rootGamePly) const;
    [[nodiscard]] bool insufficientMaterial() const;
    [[nodiscard]] bool hasNonPawnMaterial(int color) const;
    [[nodiscard]] GameResult result() const;
    [[nodiscard]] int gamePly() const { return static_cast<int>(keyHistory_.size()); }

    // Diagnostics: recomputes the hash and piece lists from the board and
    // reports whether they match the incrementally maintained copies.
    [[nodiscard]] bool consistent() const;
    [[nodiscard]] std::string ascii(bool unicode = true, bool fromBlack = false) const;

private:
    std::array<std::uint8_t, 128> board_{};
    std::array<std::array<int, 16>, 2> list_{};   // occupied squares, per colour
    std::array<int, 128> listIndex_{};            // square -> index within list_
    std::array<int, 2> count_{};                  // pieces per colour
    std::array<int, 2> kingSq_{};
    std::vector<std::uint64_t> keyHistory_;
    std::size_t repetitionStart_ = 0;           // earliest legal history after an irreversible/null move
    std::uint64_t repetitionContext_ = 0;
    int side_ = WHITE;
    int castling_ = 0;
    int epSquare_ = -1;
    int hashedEpFile_ = -1;                    // EP is hashed only if a legal capture exists
    int halfmove_ = 0;
    int fullmove_ = 1;
    std::uint64_t key_ = 0;

    void clear();
    void put(int sq, int piece);
    void restorePiece(int sq, int piece, int index);
    void remove(int sq);
    void movePiece(int from, int to);
    void recomputeKey();
    [[nodiscard]] std::uint64_t computeKey() const;
    [[nodiscard]] int legalEpFile() const;
    void generatePawn(MoveList& out, int sq, int color, bool capturesOnly) const;
    void generatePiece(MoveList& out, int sq, int type, int color, bool capturesOnly) const;
    void generateCastles(MoveList& out) const;
};

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

[[nodiscard]] int pieceValue(int type);
// Score in centipawns from the point of view of the side to move.
[[nodiscard]] int evaluate(const Position& position);

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

struct SearchLimits {
    int depth = MAX_PLY - 8;
    std::uint64_t nodes = 0;      // 0 = unlimited
    int moveTimeMs = 0;           // fixed time for this move
    int timeLeftMs[2] = {-1, -1}; // indexed by colour; -1 = not supplied
    int incrementMs[2] = {0, 0};
    int movesToGo = 0;
    bool infinite = false;
};

struct SearchReport {
    Move best{};
    Move ponder{};
    int score = 0;
    int depth = 0;
    int selDepth = 0;
    bool mate = false;
    int mateIn = 0;               // signed: positive = we mate, negative = we are mated
    std::uint64_t nodes = 0;
    std::uint64_t elapsedMs = 0;
    std::vector<Move> pv;
};

class Searcher {
public:
    Searcher();

    void setHashSizeMb(int mb);
    void clearHash();
    void newGame();
    // Call before launching a worker. A stop arriving after this preparation
    // must survive until search() observes it, even if the worker starts late.
    void prepareSearch() { stop_.store(false, std::memory_order_relaxed); }
    void stop() { stop_.store(true, std::memory_order_relaxed); }

    // Called once per completed iteration so a front end can stream `info`.
    void setInfoHandler(std::function<void(const SearchReport&)> handler) { onInfo_ = std::move(handler); }

    SearchReport search(Position& position, const SearchLimits& limits);

    [[nodiscard]] std::uint64_t nodes() const { return nodes_; }
    [[nodiscard]] int hashfullPermille() const;

private:
    friend struct SearcherTestAccess;
    class MovePicker;
    enum Bound : std::uint8_t { BOUND_NONE = 0, BOUND_EXACT = 1, BOUND_LOWER = 2, BOUND_UPPER = 3 };

    struct TTEntry {
        std::uint64_t key = 0;
        std::uint64_t repetitionContext = 0;
        std::uint64_t rootHistoryContext = 0;
        int score = 0;
        Move best{};
        std::int16_t depth = -1;
        std::uint16_t halfmoveClock = 0;
        std::uint8_t bound = BOUND_NONE;
        std::uint8_t age = 0;
    };

    std::vector<TTEntry> tt_;
    std::uint8_t age_ = 0;
    std::size_t used_ = 0;

    std::array<std::array<std::array<int, 128>, 128>, 2> history_{};
    std::array<std::array<Move, 2>, MAX_PLY + 8> killers_{};
    std::array<int, MAX_PLY + 8> evalStack_{};
    std::array<std::array<Move, MAX_PLY + 8>, MAX_PLY + 8> pvTable_{};
    std::array<int, MAX_PLY + 8> pvLength_{};

    std::atomic<bool> stop_{false};
    std::function<void(const SearchReport&)> onInfo_;
    std::uint64_t nodes_ = 0;
    std::uint64_t nodeLimit_ = 0;
    std::uint64_t startMs_ = 0;
    int softLimitMs_ = 0;
    int hardLimitMs_ = 0;
    int selDepth_ = 0;
    int rootColor_ = WHITE;
    int rootGamePly_ = 1;
    std::uint64_t rootHistoryContext_ = 0;
    bool aborted_ = false;

    [[nodiscard]] bool isDraw(const Position& position) const;
    int negamax(Position& position, int depth, int alpha, int beta, int ply, bool allowNull);
    int quiescence(Position& position, int alpha, int beta, int ply);
    bool timeUp(bool checkClock = true);
    bool enterNode(int ply);
    TTEntry* probe(std::uint64_t key);
    void store(const Position& position, int depth, int score, int bound, const Move& best, int ply);
    void setUpTiming(const Position& position, const SearchLimits& limits);
    void updatePv(int ply, const Move& move);

};

// ---------------------------------------------------------------------------
// Notation and helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string startFen();
[[nodiscard]] std::uint64_t perft(Position& position, int depth);
// Per-root-move node counts, for pinpointing a move generation divergence.
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> perftDivide(Position& position, int depth);

// Accepts UCI ("e2e4", "e7e8q") or SAN ("Nf3", "exd5", "O-O", "e8=Q+").
// Returns a null Move when the text matches no legal move.
[[nodiscard]] Move parseMove(const Position& position, const std::string& text);
[[nodiscard]] std::string toSan(const Position& position, const Move& move);
[[nodiscard]] std::string describeResult(GameResult result);

} // namespace aether
