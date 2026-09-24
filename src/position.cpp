#include "engine.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <random>
#include <sstream>

namespace aether {
namespace {

constexpr int KNIGHT_STEPS[] = {31, 33, 14, 18, -31, -33, -14, -18};
constexpr int BISHOP_STEPS[] = {15, 17, -15, -17};
constexpr int ROOK_STEPS[] = {1, -1, 16, -16};
constexpr int KING_STEPS[] = {1, -1, 16, -16, 15, 17, -15, -17};

using Board = std::array<std::uint8_t, 128>;

// Shared by normal check detection and small hypothetical boards for EP/SEE.
// In particular, EP removes two blockers, so a simple pawn-pin test is not enough.
bool squareAttacked(const Board& board, int sq, int byColor) {
    if (!onBoard(sq)) return false;
    const int pawn = makePiece(byColor, PAWN);
    for (int offset : {15, 17}) {
        const int from = sq + (byColor == WHITE ? -offset : offset);
        if (onBoard(from) && board[static_cast<std::size_t>(from)] == pawn) return true;
    }
    const int knight = makePiece(byColor, KNIGHT);
    for (int step : KNIGHT_STEPS) {
        const int from = sq + step;
        if (onBoard(from) && board[static_cast<std::size_t>(from)] == knight) return true;
    }
    for (int step : KING_STEPS) {
        const bool diagonal = step == 15 || step == 17 || step == -15 || step == -17;
        for (int from = sq + step; onBoard(from); from += step) {
            const int piece = board[static_cast<std::size_t>(from)];
            if (piece == EMPTY) continue;
            if (colorOf(piece) == byColor) {
                const int type = typeOf(piece);
                if (type == QUEEN || type == (diagonal ? BISHOP : ROOK) ||
                    (type == KING && from == sq + step)) return true;
            }
            break;
        }
    }
    return false;
}

bool attacksTarget(const Board& board, int from, int target) {
    const int piece = board[static_cast<std::size_t>(from)];
    const int type = typeOf(piece);
    if (type == PAWN) {
        const int direction = colorOf(piece) == WHITE ? 16 : -16;
        return from + direction - 1 == target || from + direction + 1 == target;
    }
    const int* steps = type == KNIGHT ? KNIGHT_STEPS : type == BISHOP ? BISHOP_STEPS :
                       type == ROOK ? ROOK_STEPS : KING_STEPS;
    const int count = type == BISHOP || type == ROOK ? 4 : 8;
    const bool sliding = type == BISHOP || type == ROOK || type == QUEEN;
    for (int i = 0; i < count; ++i) {
        for (int sq = from + steps[i]; onBoard(sq); sq += steps[i]) {
            if (sq == target) return true;
            if (!sliding || board[static_cast<std::size_t>(sq)] != EMPTY) break;
        }
    }
    return false;
}

std::uint64_t appendHistory(std::uint64_t context, std::uint64_t key) {
    // Addition preserves occurrence counts while allowing equivalent histories
    // in a different order to share TT scores. Mix before adding so that the
    // XOR relationships between Zobrist keys cannot create simple sum aliases.
    key = (key ^ (key >> 30)) * 0xBF58476D1CE4E5B9ULL;
    key = (key ^ (key >> 27)) * 0x94D049BB133111EBULL;
    key ^= key >> 31;
    return context + key;
}

struct Zobrist {
    std::array<std::array<std::uint64_t, 128>, 13> piece{};
    std::array<std::uint64_t, 16> castle{};
    std::array<std::uint64_t, 8> epFile{};
    std::uint64_t side = 0;

    Zobrist() {
        std::mt19937_64 rng(0xA37E2B9D51C4F701ULL);
        for (auto& pieces : piece) for (auto& value : pieces) value = rng();
        for (auto& value : castle) value = rng();
        for (auto& value : epFile) value = rng();
        side = rng();
    }
};

const Zobrist ZOBRIST;

char pieceChar(int piece) {
    static constexpr char chars[] = ".PNBRQKpnbrqk";
    return piece >= EMPTY && piece <= BK ? chars[piece] : '?';
}

int parsePiece(char c) {
    const std::string chars = ".PNBRQKpnbrqk";
    const std::size_t index = chars.find(c);
    return index == std::string::npos ? EMPTY : static_cast<int>(index);
}

} // namespace

std::string squareName(int sq) {
    if (!onBoard(sq)) return "--";
    std::string result;
    result += static_cast<char>('a' + fileOf(sq));
    result += static_cast<char>('1' + rankOf(sq));
    return result;
}

int parseSquare(const std::string& name) {
    if (name.size() != 2 || name[0] < 'a' || name[0] > 'h' || name[1] < '1' || name[1] > '8') return -1;
    return squareOf(name[0] - 'a', name[1] - '1');
}

std::string Move::uci() const {
    if (isNull()) return "0000";
    std::string result = squareName(from) + squareName(to);
    if (isPromotion()) result += "nbrq"[promotionType() - KNIGHT];
    return result;
}

std::string startFen() {
    return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
}

Position::Position() {
    clear();
    (void)setFen(startFen());
}

Position::Position(const std::string& fenText) {
    clear();
    if (!setFen(fenText)) (void)setFen(startFen());
}

void Position::clear() {
    board_.fill(EMPTY);
    for (auto& pieces : list_) pieces.fill(-1);
    listIndex_.fill(-1);
    count_.fill(0);
    kingSq_.fill(-1);
    keyHistory_.clear();
    repetitionStart_ = 0;
    repetitionContext_ = 0;
    side_ = WHITE;
    castling_ = 0;
    epSquare_ = -1;
    hashedEpFile_ = -1;
    halfmove_ = 0;
    fullmove_ = 1;
    key_ = 0;
}

void Position::put(int sq, int piece) {
    if (!onBoard(sq) || piece == EMPTY) return;
    const int color = colorOf(piece);
    const int index = count_[color];
    if (index >= static_cast<int>(list_[color].size())) return;
    board_[static_cast<std::size_t>(sq)] = static_cast<std::uint8_t>(piece);
    key_ ^= ZOBRIST.piece[static_cast<std::size_t>(piece)][static_cast<std::size_t>(sq)];
    list_[color][static_cast<std::size_t>(index)] = sq;
    listIndex_[static_cast<std::size_t>(sq)] = index;
    ++count_[color];
    if (typeOf(piece) == KING) kingSq_[color] = sq;
}

void Position::restorePiece(int sq, int piece, int index) {
    put(sq, piece);
    const int color = colorOf(piece);
    const int last = count_[color] - 1;
    const int swappedSquare = list_[color][static_cast<std::size_t>(index)];
    std::swap(list_[color][static_cast<std::size_t>(index)], list_[color][static_cast<std::size_t>(last)]);
    listIndex_[static_cast<std::size_t>(sq)] = index;
    listIndex_[static_cast<std::size_t>(swappedSquare)] = last;
}

void Position::remove(int sq) {
    if (!onBoard(sq)) return;
    const int piece = board_[static_cast<std::size_t>(sq)];
    if (piece == EMPTY) return;
    const int color = colorOf(piece);
    const int index = listIndex_[static_cast<std::size_t>(sq)];
    const int lastIndex = --count_[color];
    const int lastSquare = list_[color][static_cast<std::size_t>(lastIndex)];
    if (index != lastIndex) {
        list_[color][static_cast<std::size_t>(index)] = lastSquare;
        listIndex_[static_cast<std::size_t>(lastSquare)] = index;
    }
    list_[color][static_cast<std::size_t>(lastIndex)] = -1;
    listIndex_[static_cast<std::size_t>(sq)] = -1;
    board_[static_cast<std::size_t>(sq)] = EMPTY;
    key_ ^= ZOBRIST.piece[static_cast<std::size_t>(piece)][static_cast<std::size_t>(sq)];
    if (typeOf(piece) == KING) kingSq_[color] = -1;
}

void Position::movePiece(int from, int to) {
    const int piece = board_[static_cast<std::size_t>(from)];
    const int color = colorOf(piece);
    const int index = listIndex_[static_cast<std::size_t>(from)];
    board_[static_cast<std::size_t>(from)] = EMPTY;
    board_[static_cast<std::size_t>(to)] = static_cast<std::uint8_t>(piece);
    key_ ^= ZOBRIST.piece[static_cast<std::size_t>(piece)][static_cast<std::size_t>(from)] ^
            ZOBRIST.piece[static_cast<std::size_t>(piece)][static_cast<std::size_t>(to)];
    list_[color][static_cast<std::size_t>(index)] = to;
    listIndex_[static_cast<std::size_t>(from)] = -1;
    listIndex_[static_cast<std::size_t>(to)] = index;
    if (typeOf(piece) == KING) kingSq_[color] = to;
}

bool Position::setFen(const std::string& fenText) {
    const Position backup = *this;
    const auto reject = [&]() { *this = backup; return false; };
    clear();
    std::istringstream input(fenText);
    std::string placement, turn, castles, ep;
    if (!(input >> placement >> turn >> castles >> ep)) return reject();
    // Keep four-field FEN support, but reject malformed or overflowing counters
    // instead of silently replacing them with defaults.
    const auto parseCounter = [](const std::string& token, int& result) {
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
    };
    std::string counter;
    if (input >> counter) {
        if (!parseCounter(counter, halfmove_)) return reject();
        if (input >> counter) {
            if (!parseCounter(counter, fullmove_)) return reject();
            if (input >> counter) return reject();
        }
    }
    if (halfmove_ < 0 || fullmove_ < 1 || (turn != "w" && turn != "b")) return reject();

    int rank = 7;
    int file = 0;
    std::array<int, 2> pawns{};
    for (char c : placement) {
        if (c == '/') {
            if (file != 8 || rank == 0) return reject();
            --rank;
            file = 0;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            const int empty = c - '0';
            if (empty < 1 || empty > 8 || file + empty > 8) return reject();
            file += empty;
        } else {
            const int piece = parsePiece(c);
            if (piece == EMPTY || file >= 8 || count_[colorOf(piece)] >= 16) return reject();
            const int color = colorOf(piece);
            if (typeOf(piece) == KING && kingSq_[color] >= 0) return reject();
            if (typeOf(piece) == PAWN && (rank == 0 || rank == 7 || ++pawns[color] > 8)) return reject();
            put(squareOf(file++, rank), piece);
        }
    }
    if (rank != 0 || file != 8 || kingSq_[WHITE] < 0 || kingSq_[BLACK] < 0) return reject();
    for (int step : KING_STEPS) if (kingSq_[WHITE] + step == kingSq_[BLACK]) return reject();

    side_ = turn == "w" ? WHITE : BLACK;
    castling_ = 0;
    if (castles != "-") {
        for (char c : castles) {
            const int right = c == 'K' ? WHITE_OO : c == 'Q' ? WHITE_OOO :
                              c == 'k' ? BLACK_OO : c == 'q' ? BLACK_OOO : 0;
            if (right == 0 || (castling_ & right) != 0) return reject();
            castling_ |= right;
        }
    }
    if (((castling_ & (WHITE_OO | WHITE_OOO)) != 0 && pieceAt(4) != WK) ||
        ((castling_ & (BLACK_OO | BLACK_OOO)) != 0 && pieceAt(116) != BK) ||
        ((castling_ & WHITE_OO) != 0 && pieceAt(7) != WR) ||
        ((castling_ & WHITE_OOO) != 0 && pieceAt(0) != WR) ||
        ((castling_ & BLACK_OO) != 0 && pieceAt(119) != BR) ||
        ((castling_ & BLACK_OOO) != 0 && pieceAt(112) != BR)) return reject();
    epSquare_ = ep == "-" ? -1 : parseSquare(ep);
    if (ep != "-" && epSquare_ < 0) return reject();
    if (epSquare_ >= 0) {
        const int direction = side_ == WHITE ? 16 : -16;
        if (rankOf(epSquare_) != (side_ == WHITE ? 5 : 2) || pieceAt(epSquare_) != EMPTY ||
            pieceAt(epSquare_ - direction) != makePiece(side_ ^ 1, PAWN) ||
            pieceAt(epSquare_ + direction) != EMPTY) return reject();
    }

    recomputeKey();
    keyHistory_.push_back(key_);
    repetitionContext_ = appendHistory(0, key_);
    return true;
}

std::uint64_t Position::computeKey() const {
    std::uint64_t result = 0;
    for (int sq = 0; sq < 128; ++sq) {
        if (!onBoard(sq)) { sq += 7; continue; }
        const int piece = board_[static_cast<std::size_t>(sq)];
        if (piece != EMPTY) result ^= ZOBRIST.piece[static_cast<std::size_t>(piece)][static_cast<std::size_t>(sq)];
    }
    result ^= ZOBRIST.castle[static_cast<std::size_t>(castling_)];
    const int epFile = legalEpFile();
    if (epFile >= 0) result ^= ZOBRIST.epFile[static_cast<std::size_t>(epFile)];
    if (side_ == BLACK) result ^= ZOBRIST.side;
    return result;
}

void Position::recomputeKey() {
    hashedEpFile_ = legalEpFile();
    key_ = computeKey();
}

int Position::legalEpFile() const {
    if (epSquare_ < 0) return -1;
    const int direction = side_ == WHITE ? 16 : -16;
    const int captureSquare = epSquare_ - direction;
    if (!onBoard(epSquare_) || !onBoard(captureSquare) || pieceAt(epSquare_) != EMPTY ||
        pieceAt(captureSquare) != makePiece(side_ ^ 1, PAWN)) return -1;
    Board after = board_;
    after[static_cast<std::size_t>(captureSquare)] = EMPTY;
    after[static_cast<std::size_t>(epSquare_)] = static_cast<std::uint8_t>(makePiece(side_, PAWN));
    for (int from : {captureSquare - 1, captureSquare + 1}) {
        if (!onBoard(from) || pieceAt(from) != makePiece(side_, PAWN)) continue;
        after[static_cast<std::size_t>(from)] = EMPTY;
        if (!squareAttacked(after, kingSq_[side_], side_ ^ 1)) return fileOf(epSquare_);
        after[static_cast<std::size_t>(from)] = static_cast<std::uint8_t>(makePiece(side_, PAWN));
    }
    return -1;
}

std::string Position::fen() const {
    std::ostringstream output;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const int piece = pieceAt(squareOf(file, rank));
            if (piece == EMPTY) ++empty;
            else {
                if (empty != 0) output << empty;
                empty = 0;
                output << pieceChar(piece);
            }
        }
        if (empty != 0) output << empty;
        if (rank != 0) output << '/';
    }
    output << ' ' << (side_ == WHITE ? 'w' : 'b') << ' ';
    if (castling_ == 0) output << '-';
    else {
        if ((castling_ & WHITE_OO) != 0) output << 'K';
        if ((castling_ & WHITE_OOO) != 0) output << 'Q';
        if ((castling_ & BLACK_OO) != 0) output << 'k';
        if ((castling_ & BLACK_OOO) != 0) output << 'q';
    }
    output << ' ' << (epSquare_ < 0 ? "-" : squareName(epSquare_));
    output << ' ' << halfmove_ << ' ' << fullmove_;
    return output.str();
}

bool Position::isSquareAttacked(int sq, int byColor) const {
    return squareAttacked(board_, sq, byColor);
}

void Position::generatePawn(MoveList& out, int sq, int color, bool capturesOnly) const {
    const int direction = color == WHITE ? 16 : -16;
    const int startRank = color == WHITE ? 1 : 6;
    const int promotionRank = color == WHITE ? 6 : 1;
    const int forward = sq + direction;
    if (onBoard(forward) && pieceAt(forward) == EMPTY) {
        if (rankOf(sq) == promotionRank) {
            for (int flag = PROMOTE_N; flag <= PROMOTE_Q; ++flag) out.add(sq, forward, flag);
        } else if (!capturesOnly) {
            out.add(sq, forward);
            if (rankOf(sq) == startRank && pieceAt(sq + 2 * direction) == EMPTY) out.add(sq, sq + 2 * direction, DOUBLE_PUSH);
        }
    }
    for (int fileStep : {-1, 1}) {
        const int to = sq + direction + fileStep;
        if (!onBoard(to)) continue;
        const int target = pieceAt(to);
        const int epCapture = to - direction;
        const bool capture = target != EMPTY && colorOf(target) != color && typeOf(target) != KING;
        const bool ep = target == EMPTY && to == epSquare_ && onBoard(epCapture) &&
                        pieceAt(epCapture) == makePiece(color ^ 1, PAWN);
        if (!capture && !ep) continue;
        if (rankOf(sq) == promotionRank) for (int flag = PROMOTE_N_CAPTURE; flag <= PROMOTE_Q_CAPTURE; ++flag) out.add(sq, to, flag);
        else out.add(sq, to, ep ? EP_CAPTURE : CAPTURE);
    }
}

void Position::generatePiece(MoveList& out, int sq, int type, int color, bool capturesOnly) const {
    if (type == QUEEN) {
        generatePiece(out, sq, BISHOP, color, capturesOnly);
        generatePiece(out, sq, ROOK, color, capturesOnly);
        return;
    }
    const int* steps = type == KNIGHT ? KNIGHT_STEPS : type == BISHOP ? BISHOP_STEPS : type == ROOK ? ROOK_STEPS : KING_STEPS;
    const int stepCount = (type == KNIGHT || type == KING) ? 8 : 4;
    const bool sliding = type == BISHOP || type == ROOK;
    for (int i = 0; i < stepCount; ++i) {
        for (int to = sq + steps[i]; onBoard(to); to += steps[i]) {
            const int target = pieceAt(to);
            if (target == EMPTY) {
                if (!capturesOnly) out.add(sq, to);
            } else {
                if (colorOf(target) != color && typeOf(target) != KING) out.add(sq, to, CAPTURE);
                break;
            }
            if (!sliding) break;
        }
    }
}

void Position::generateCastles(MoveList& out) const {
    if (side_ == WHITE && kingSq_[WHITE] == 4) {
        if ((castling_ & WHITE_OO) != 0 && pieceAt(7) == WR && pieceAt(5) == EMPTY && pieceAt(6) == EMPTY &&
            !isSquareAttacked(4, BLACK) && !isSquareAttacked(5, BLACK) && !isSquareAttacked(6, BLACK)) out.add(4, 6, KING_CASTLE);
        if ((castling_ & WHITE_OOO) != 0 && pieceAt(0) == WR && pieceAt(1) == EMPTY && pieceAt(2) == EMPTY && pieceAt(3) == EMPTY &&
            !isSquareAttacked(4, BLACK) && !isSquareAttacked(3, BLACK) && !isSquareAttacked(2, BLACK)) out.add(4, 2, QUEEN_CASTLE);
    } else if (side_ == BLACK && kingSq_[BLACK] == 116) {
        if ((castling_ & BLACK_OO) != 0 && pieceAt(119) == BR && pieceAt(117) == EMPTY && pieceAt(118) == EMPTY &&
            !isSquareAttacked(116, WHITE) && !isSquareAttacked(117, WHITE) && !isSquareAttacked(118, WHITE)) out.add(116, 118, KING_CASTLE);
        if ((castling_ & BLACK_OOO) != 0 && pieceAt(112) == BR && pieceAt(113) == EMPTY && pieceAt(114) == EMPTY && pieceAt(115) == EMPTY &&
            !isSquareAttacked(116, WHITE) && !isSquareAttacked(115, WHITE) && !isSquareAttacked(114, WHITE)) out.add(116, 114, QUEEN_CASTLE);
    }
}

void Position::generate(MoveList& out, bool capturesOnly) const {
    out.clear();
    for (int i = 0; i < count_[side_]; ++i) {
        const int sq = list_[side_][static_cast<std::size_t>(i)];
        const int type = typeOf(pieceAt(sq));
        if (type == PAWN) generatePawn(out, sq, side_, capturesOnly);
        else generatePiece(out, sq, type, side_, capturesOnly);
    }
    if (!capturesOnly) generateCastles(out);
}

std::vector<Move> Position::legalMoves(bool capturesOnly) const {
    MoveList pseudo;
    generate(pseudo, capturesOnly);
    std::vector<Move> result;
    result.reserve(static_cast<std::size_t>(pseudo.size()));
    Position work = *this;
    for (const Move& move : pseudo) {
        Undo undo;
        if (work.makeMove(move, undo)) {
            result.push_back(move);
            work.unmakeMove(move, undo);
        }
    }
    return result;
}

bool Position::hasLegalMove() const {
    MoveList moves;
    generate(moves);
    return hasLegalMove(moves);
}

bool Position::hasLegalMove(const MoveList& candidates) const {
    for (const Move& move : candidates) {
        // generate() already checks movement geometry and castle transit
        // squares. Only the final occupancy and our king's safety remain.
        Board after = board_;
        const int moving = after[move.from];
        after[move.from] = EMPTY;
        if (move.flag == EP_CAPTURE) after[move.to + (side_ == WHITE ? -16 : 16)] = EMPTY;
        after[move.to] = static_cast<std::uint8_t>(move.isPromotion()
            ? makePiece(side_, move.promotionType()) : moving);
        if (move.isCastle()) {
            const int rank = side_ == WHITE ? 0 : 7;
            const bool kingSide = move.flag == KING_CASTLE;
            after[squareOf(kingSide ? 7 : 0, rank)] = EMPTY;
            after[squareOf(kingSide ? 5 : 3, rank)] = static_cast<std::uint8_t>(makePiece(side_, ROOK));
        }
        const int king = typeOf(moving) == KING ? move.to : kingSq_[side_];
        if (!squareAttacked(after, king, side_ ^ 1)) return true;
    }
    return false;
}

bool Position::makeMove(const Move& move, Undo& undo) {
    if (move.from == move.to || !onBoard(move.from) || !onBoard(move.to) ||
        move.flag > PROMOTE_Q_CAPTURE || move.flag == 6 || move.flag == 7) return false;
    const int moving = pieceAt(move.from);
    if (moving == EMPTY || colorOf(moving) != side_) return false;
    const int mover = side_;
    const int target = pieceAt(move.to);
    if (target != EMPTY && (colorOf(target) == mover || typeOf(target) == KING)) return false;
    const int direction = mover == WHITE ? 16 : -16;
    if (move.flag == EP_CAPTURE) {
        if (typeOf(moving) != PAWN || move.to != epSquare_ || target != EMPTY ||
            (move.to != move.from + direction - 1 && move.to != move.from + direction + 1) ||
            !onBoard(move.to - direction) || pieceAt(move.to - direction) != makePiece(mover ^ 1, PAWN)) return false;
    } else if (move.isCapture() != (target != EMPTY)) return false;
    if (move.isPromotion()) {
        if (typeOf(moving) != PAWN || rankOf(move.from) != (mover == WHITE ? 6 : 1) ||
            rankOf(move.to) != (mover == WHITE ? 7 : 0)) return false;
    } else if (typeOf(moving) == PAWN && (rankOf(move.to) == 0 || rankOf(move.to) == 7)) return false;
    if (move.flag == DOUBLE_PUSH && (typeOf(moving) != PAWN ||
        rankOf(move.from) != (mover == WHITE ? 1 : 6) || move.to != move.from + 2 * direction ||
        pieceAt(move.from + direction) != EMPTY)) return false;
    if (move.isCastle()) {
        MoveList castles;
        generateCastles(castles);
        if (std::find(castles.begin(), castles.end(), move) == castles.end()) return false;
    }
    undo.moved = moving;
    undo.movedIndex = listIndex_[move.from];
    undo.castling = castling_;
    undo.epSquare = epSquare_;
    undo.hashedEpFile = hashedEpFile_;
    undo.halfmove = halfmove_;
    undo.fullmove = fullmove_;
    undo.key = key_;
    undo.historySize = keyHistory_.size();
    undo.repetitionStart = repetitionStart_;
    undo.repetitionContext = repetitionContext_;
    undo.captureSquare = move.flag == EP_CAPTURE ? move.to - direction : move.to;
    undo.captured = pieceAt(undo.captureSquare);
    undo.capturedIndex = listIndex_[static_cast<std::size_t>(undo.captureSquare)];

    // Piece helpers XOR only the changed squares; state terms are toggled here.
    if (hashedEpFile_ >= 0) key_ ^= ZOBRIST.epFile[static_cast<std::size_t>(hashedEpFile_)];
    key_ ^= ZOBRIST.castle[static_cast<std::size_t>(castling_)];
    if (undo.captured != EMPTY) remove(undo.captureSquare);

    if (move.isPromotion()) {
        remove(move.from);
        put(move.to, makePiece(mover, move.promotionType()));
    } else movePiece(move.from, move.to);
    if (move.flag == KING_CASTLE) movePiece(mover == WHITE ? 7 : 119, mover == WHITE ? 5 : 117);
    else if (move.flag == QUEEN_CASTLE) movePiece(mover == WHITE ? 0 : 112, mover == WHITE ? 3 : 115);

    if (typeOf(moving) == KING) castling_ &= mover == WHITE ? ~(WHITE_OO | WHITE_OOO) : ~(BLACK_OO | BLACK_OOO);
    if (move.from == 0 || move.to == 0) castling_ &= ~WHITE_OOO;
    if (move.from == 7 || move.to == 7) castling_ &= ~WHITE_OO;
    if (move.from == 112 || move.to == 112) castling_ &= ~BLACK_OOO;
    if (move.from == 119 || move.to == 119) castling_ &= ~BLACK_OO;
    epSquare_ = move.flag == DOUBLE_PUSH ? move.from + direction : -1;
    halfmove_ = typeOf(moving) == PAWN || undo.captured != EMPTY ? 0 :
                halfmove_ + (halfmove_ < std::numeric_limits<int>::max());
    if (mover == BLACK && fullmove_ < std::numeric_limits<int>::max()) ++fullmove_;
    side_ ^= 1;
    key_ ^= ZOBRIST.side ^ ZOBRIST.castle[static_cast<std::size_t>(castling_)];
    hashedEpFile_ = legalEpFile();
    if (hashedEpFile_ >= 0) key_ ^= ZOBRIST.epFile[static_cast<std::size_t>(hashedEpFile_)];

    if (kingSq_[mover] < 0 || isSquareAttacked(kingSq_[mover], side_)) {
        unmakeMove(move, undo);
        return false;
    }
    keyHistory_.push_back(key_);
    if (halfmove_ == 0 || castling_ != undo.castling) {
        repetitionStart_ = keyHistory_.size() - 1;
        repetitionContext_ = appendHistory(0, key_);
    } else repetitionContext_ = appendHistory(repetitionContext_, key_);
    return true;
}

void Position::unmakeMove(const Move& move, const Undo& undo) {
    // The saved length also works for an illegal move rejected before push_back.
    keyHistory_.resize(undo.historySize);
    repetitionStart_ = undo.repetitionStart;
    repetitionContext_ = undo.repetitionContext;
    side_ ^= 1;
    const int mover = side_;
    fullmove_ = undo.fullmove;
    if (move.flag == KING_CASTLE) movePiece(mover == WHITE ? 5 : 117, mover == WHITE ? 7 : 119);
    else if (move.flag == QUEEN_CASTLE) movePiece(mover == WHITE ? 3 : 115, mover == WHITE ? 0 : 112);
    if (move.isPromotion()) {
        remove(move.to);
        restorePiece(move.from, undo.moved, undo.movedIndex);
    } else movePiece(move.to, move.from);
    if (undo.captured != EMPTY) restorePiece(undo.captureSquare, undo.captured, undo.capturedIndex);
    castling_ = undo.castling;
    epSquare_ = undo.epSquare;
    hashedEpFile_ = undo.hashedEpFile;
    halfmove_ = undo.halfmove;
    key_ = undo.key;
}

void Position::makeNullMove(Undo& undo) {
    undo.castling = castling_;
    undo.epSquare = epSquare_;
    undo.hashedEpFile = hashedEpFile_;
    undo.halfmove = halfmove_;
    undo.fullmove = fullmove_;
    undo.key = key_;
    undo.historySize = keyHistory_.size();
    undo.repetitionStart = repetitionStart_;
    undo.repetitionContext = repetitionContext_;
    if (hashedEpFile_ >= 0) key_ ^= ZOBRIST.epFile[static_cast<std::size_t>(hashedEpFile_)];
    epSquare_ = -1;
    hashedEpFile_ = -1;
    side_ ^= 1;
    key_ ^= ZOBRIST.side;
    // A synthetic pass is neither a reversible game move nor a draw claim.
    // Its descendants must not find repetitions on the other side of the pass.
    repetitionStart_ = keyHistory_.size();
    repetitionContext_ = 0;
}

void Position::unmakeNullMove(const Undo& undo) {
    side_ ^= 1;
    castling_ = undo.castling;
    epSquare_ = undo.epSquare;
    hashedEpFile_ = undo.hashedEpFile;
    halfmove_ = undo.halfmove;
    fullmove_ = undo.fullmove;
    key_ = undo.key;
    keyHistory_.resize(undo.historySize);
    repetitionStart_ = undo.repetitionStart;
    repetitionContext_ = undo.repetitionContext;
}

bool Position::givesCheck(const Move& move) const {
    Position copy = *this;
    Undo undo;
    return copy.makeMove(move, undo) && copy.inCheck();
}

int Position::see(const Move& move) const {
    if (!move.isCapture() && !move.isPromotion()) return 0;
    if (!onBoard(move.from) || !onBoard(move.to)) return 0;
    const int moving = pieceAt(move.from);
    if (moving == EMPTY || colorOf(moving) != side_) return 0;
    const int capturedSquare = move.flag == EP_CAPTURE ? move.to + (side_ == WHITE ? -16 : 16) : move.to;
    if (!onBoard(capturedSquare)) return 0;
    Board after = board_;
    auto kings = kingSq_;
    after[move.from] = EMPTY;
    after[static_cast<std::size_t>(capturedSquare)] = EMPTY;
    after[move.to] = static_cast<std::uint8_t>(move.isPromotion() ? makePiece(side_, move.promotionType()) : moving);
    if (typeOf(moving) == KING) kings[side_] = move.to;
    if (squareAttacked(after, kings[side_], side_ ^ 1)) return -VALUE_INF;
    std::array<int, 32> gains{};
    gains[0] = pieceValue(typeOf(pieceAt(capturedSquare)));
    if (move.isPromotion()) gains[0] += pieceValue(move.promotionType()) - pieceValue(PAWN);
    int depth = 0;
    int color = side_ ^ 1;
    while (depth + 1 < static_cast<int>(gains.size()) && typeOf(after[move.to]) != KING) {
        const int onTarget = after[move.to];
        int attacker = -1;
        int bestValue = VALUE_INF;
        int arriving = EMPTY;
        // Use piece lists to find candidates, then test the updated occupancy.
        // This excludes absolute pins and unsafe king captures as x-rays open.
        for (int i = 0; i < count_[color]; ++i) {
            const int from = list_[color][static_cast<std::size_t>(i)];
            const int piece = after[static_cast<std::size_t>(from)];
            if (from == move.to || piece == EMPTY || colorOf(piece) != color) continue;
            const int type = typeOf(piece);
            const int value = type == KING ? VALUE_INF - 1 : pieceValue(type);
            if (value >= bestValue || !attacksTarget(after, from, move.to)) continue;
            const int replacement = type == PAWN && rankOf(move.to) == (color == WHITE ? 7 : 0) ?
                                    makePiece(color, QUEEN) : piece;
            after[static_cast<std::size_t>(from)] = EMPTY;
            after[move.to] = static_cast<std::uint8_t>(replacement);
            const bool legal = !squareAttacked(after, type == KING ? move.to : kings[color], color ^ 1);
            after[static_cast<std::size_t>(from)] = static_cast<std::uint8_t>(piece);
            after[move.to] = static_cast<std::uint8_t>(onTarget);
            if (legal) {
                attacker = from;
                bestValue = value;
                arriving = replacement;
            }
        }
        if (attacker < 0) break;
        const int attackingType = typeOf(after[static_cast<std::size_t>(attacker)]);
        const int promotionGain = attackingType == PAWN && typeOf(arriving) == QUEEN ?
                                  pieceValue(QUEEN) - pieceValue(PAWN) : 0;
        ++depth;
        gains[static_cast<std::size_t>(depth)] = pieceValue(typeOf(onTarget)) + promotionGain - gains[static_cast<std::size_t>(depth - 1)];
        after[static_cast<std::size_t>(attacker)] = EMPTY;
        after[move.to] = static_cast<std::uint8_t>(arriving);
        if (attackingType == KING) kings[color] = move.to;
        color ^= 1;
    }
    while (depth > 0) {
        gains[static_cast<std::size_t>(depth - 1)] = -std::max(-gains[static_cast<std::size_t>(depth - 1)], gains[static_cast<std::size_t>(depth)]);
        --depth;
    }
    return gains[0];
}

bool Position::isRepetition(int minCount) const {
    if (minCount <= 0) return true;
    if (keyHistory_.size() < 3 || repetitionStart_ >= keyHistory_.size()) return false;
    int matches = 0;
    const std::size_t current = keyHistory_.size() - 1;
    const std::size_t reversible = std::min(static_cast<std::size_t>(halfmove_), current);
    const std::size_t first = std::max(current - reversible, repetitionStart_);
    for (std::size_t distance = 2; distance <= current - first; distance += 2) {
        if (keyHistory_[current - distance] == key_ && ++matches >= minCount) return true;
    }
    return false;
}

std::uint64_t Position::repetitionContextBeforeCurrent() const {
    // Null moves are deliberately absent from the legal history.
    if (repetitionStart_ >= keyHistory_.size()) return 0;
    return repetitionContext_ - appendHistory(0, key_);
}

bool Position::isSearchRepetition(int rootGamePly) const {
    if (keyHistory_.size() < 3 || repetitionStart_ >= keyHistory_.size()) return false;
    const std::size_t current = keyHistory_.size() - 1;
    const std::size_t root = static_cast<std::size_t>(std::max(0, rootGamePly - 1));
    const std::size_t reversible = std::min(static_cast<std::size_t>(halfmove_), current);
    const std::size_t first = std::max(current - reversible, repetitionStart_);
    int matches = 0;
    for (std::size_t distance = 2; distance <= current - first; distance += 2) {
        const std::size_t previous = current - distance;
        if (keyHistory_[previous] != key_) continue;
        if (previous >= root || ++matches >= 2) return true;
    }
    return false;
}

bool Position::hasNonPawnMaterial(int color) const {
    for (int i = 0; i < count_[color]; ++i) {
        const int type = typeOf(pieceAt(list_[color][static_cast<std::size_t>(i)]));
        if (type != PAWN && type != KING) return true;
    }
    return false;
}

bool Position::insufficientMaterial() const {
    int minors = 0;
    int bishops = 0;
    int bishopComplex = -1;
    for (int color = WHITE; color <= BLACK; ++color) {
        for (int i = 0; i < count_[color]; ++i) {
            const int sq = list_[color][static_cast<std::size_t>(i)];
            const int type = typeOf(pieceAt(sq));
            if (type == PAWN || type == ROOK || type == QUEEN) return false;
            if (type == KNIGHT) ++minors;
            if (type == BISHOP) {
                ++minors;
                ++bishops;
                const int complex = (fileOf(sq) + rankOf(sq)) & 1;
                if (bishopComplex < 0) bishopComplex = complex;
                else if (bishopComplex != complex) bishopComplex = 2;
            }
        }
    }
    return minors <= 1 || (bishops == minors && bishopComplex != 2);
}

GameResult Position::result() const {
    // Mate ends the game immediately, including on the hundredth halfmove.
    if (!hasLegalMove()) {
        if (!inCheck()) return GameResult::DrawStalemate;
        return side_ == WHITE ? GameResult::BlackWins : GameResult::WhiteWins;
    }
    if (halfmove_ >= 100) return GameResult::DrawFiftyMove;
    if (isRepetition()) return GameResult::DrawRepetition;
    if (insufficientMaterial()) return GameResult::DrawMaterial;
    return GameResult::Ongoing;
}

bool Position::consistent() const {
    if (computeKey() != key_ || legalEpFile() != hashedEpFile_) return false;
    if (keyHistory_.empty() || repetitionStart_ > keyHistory_.size()) return false;
    if (repetitionStart_ < keyHistory_.size() && keyHistory_.back() != key_) return false;
    std::uint64_t context = 0;
    for (std::size_t i = repetitionStart_; i < keyHistory_.size(); ++i) context = appendHistory(context, keyHistory_[i]);
    if (context != repetitionContext_) return false;
    std::array<int, 2> boardCounts{};
    std::array<int, 2> kings{-1, -1};
    std::array<bool, 128> listed{};
    for (int color = WHITE; color <= BLACK; ++color) {
        if (count_[color] < 1 || count_[color] > 16) return false;
        for (int i = 0; i < count_[color]; ++i) {
            const int sq = list_[color][static_cast<std::size_t>(i)];
            if (!onBoard(sq) || listed[static_cast<std::size_t>(sq)] || pieceAt(sq) == EMPTY || colorOf(pieceAt(sq)) != color) return false;
            if (listIndex_[static_cast<std::size_t>(sq)] != i) return false;
            listed[static_cast<std::size_t>(sq)] = true;
        }
    }
    for (int sq = 0; sq < 128; ++sq) {
        if (!onBoard(sq)) { sq += 7; continue; }
        const int piece = pieceAt(sq);
        if (piece == EMPTY) {
            if (listIndex_[static_cast<std::size_t>(sq)] != -1) return false;
            continue;
        }
        const int color = colorOf(piece);
        ++boardCounts[color];
        if (!listed[static_cast<std::size_t>(sq)]) return false;
        if (typeOf(piece) == PAWN && (rankOf(sq) == 0 || rankOf(sq) == 7)) return false;
        if (typeOf(piece) == KING) {
            if (kings[color] >= 0) return false;
            kings[color] = sq;
        }
    }
    return boardCounts == count_ && kings == kingSq_;
}

std::string Position::ascii(bool, bool fromBlack) const {
    std::ostringstream output;
    for (int row = 0; row < 8; ++row) {
        const int rank = fromBlack ? row : 7 - row;
        output << rank + 1 << "  ";
        for (int column = 0; column < 8; ++column) {
            const int file = fromBlack ? 7 - column : column;
            output << pieceChar(pieceAt(squareOf(file, rank))) << ' ';
        }
        output << '\n';
    }
    output << "   ";
    for (int column = 0; column < 8; ++column) output << static_cast<char>('a' + (fromBlack ? 7 - column : column)) << ' ';
    return output.str();
}

} // namespace aether
