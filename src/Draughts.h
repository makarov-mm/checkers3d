#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The rules of draughts on the 8×8 board in three variants: Russian draughts, English draughts (checkers)
// and giveaway. Positions, moves, notation, and a game with its history.
namespace draughts {

enum class Color : std::uint8_t { White, Black };

constexpr Color Opposite(Color color) { return color == Color::White ? Color::Black : Color::White; }

enum class Variant : std::uint8_t { Russian, English, Giveaway };
constexpr int VariantCount = 3;

struct Rules {
    bool flyingKings;    // kings move and take at any distance along a diagonal
    bool menTakeBack;    // men take backwards as well as forwards
    bool crownedTakesOn; // a man crowned in the middle of a capture goes on taking, as a king
    Color first;         // who moves first
    bool giveaway;       // the side left without a move wins
    bool loneKingsDraw;  // a king against a king is a draw
    int quietLimit;      // plies of king moves with nothing taken that draw the game
};

const Rules& RulesOf(Variant variant);

// The 32 dark squares: 0 is a1, 1 c1, 2 e1, 3 g1, 4 b2 ... 31 h8.
using Square = int;
constexpr int SquareCount = 32;

constexpr int RankOf(Square square) { return square >> 2; }
constexpr int FileOf(Square square) { return ((square & 3) << 1) | (RankOf(square) & 1); }
constexpr bool IsDark(int file, int rank) { return file >= 0 && file < 8 && rank >= 0 && rank < 8 && ((file + rank) & 1) == 0; }
// The dark square at a file and rank, or -1 for a light square or one off the board.
constexpr Square SquareAt(int file, int rank) { return IsDark(file, rank) ? rank * 4 + file / 2 : -1; }
constexpr std::uint32_t Bit(Square square) { return std::uint32_t{1} << square; }

// "c3" in Russian draughts and giveaway; in English draughts the squares are numbered 1 to 32 from
// Black's side, so c3 is "22".
std::string SquareName(Square square, Variant variant);
// Either form.
std::optional<Square> ParseSquare(std::string_view text);

struct Move {
    std::uint8_t from = 0;
    std::uint8_t steps = 0;               // landings: 1 for a simple move or a single capture
    bool crowns = false;                  // a man that becomes a king
    std::uint32_t captured = 0;           // the squares of the pieces taken
    std::array<std::uint8_t, 12> path{};  // the landing squares in order; the last is where the piece stops
    std::array<std::uint8_t, 12> taken{}; // the pieces taken in order: taken[i] is jumped on the way to path[i]

    Square To() const { return path[steps - 1]; }
    bool IsNull() const { return steps == 0; }
    bool IsCapture() const { return captured != 0; }
    int CaptureCount() const { return std::popcount(captured); }

    // Moves from the same square to the same square, taking the same pieces, come to the same thing.
    bool operator==(const Move& other) const
    {
        if (IsNull() || other.IsNull())
            return IsNull() && other.IsNull();
        return from == other.from && To() == other.To() && captured == other.captured;
    }
};

class MoveList {
public:
    static constexpr int Capacity = 256;

    void Add(const Move& move)
    {
        if (m_count < Capacity)
            m_moves[m_count++] = move;
    }
    void Clear() { m_count = 0; }
    int Size() const { return m_count; }
    bool Empty() const { return m_count == 0; }
    Move& operator[](int index) { return m_moves[index]; }
    const Move& operator[](int index) const { return m_moves[index]; }
    Move* begin() { return m_moves.data(); }
    Move* end() { return m_moves.data() + m_count; }
    const Move* begin() const { return m_moves.data(); }
    const Move* end() const { return m_moves.data() + m_count; }

private:
    std::array<Move, Capacity> m_moves{};
    int m_count = 0;
};

struct Piece {
    Color color = Color::White;
    bool king = false;
};

class Position {
public:
    static Position Start(Variant variant);
    // A PDN position: "W:W21,22,K30:B1-12", or with the squares named, "W:Wc3,Ka1:Bb8".
    static std::expected<Position, std::string> FromFen(std::string_view fen);
    std::string ToFen(Variant variant) const;

    Color SideToMove() const { return m_side; }
    std::uint32_t Pieces(Color color) const { return color == Color::White ? m_white : m_black; }
    std::uint32_t Men(Color color) const { return Pieces(color) & ~m_kings; }
    std::uint32_t Kings() const { return m_kings; }
    std::uint32_t Kings(Color color) const { return Pieces(color) & m_kings; }
    std::uint32_t Occupied() const { return m_white | m_black; }
    std::optional<Piece> At(Square square) const;
    // King moves with nothing taken, in a row.
    int QuietPlies() const { return m_quietPlies; }
    std::uint64_t Key() const { return m_key; }

    // The legal moves: when something can be taken, only the captures.
    void GenerateMoves(const Rules& rules, MoveList& moves) const;
    bool HasCapture(const Rules& rules) const;
    // The pieces of the side to move that can take something.
    std::uint32_t PiecesThatCanTake(const Rules& rules) const;
    Position After(const Move& move) const;

private:
    bool CanTake(const Rules& rules, Square square) const;
    void Apply(const Move& move);
    void ComputeKey();

    std::uint32_t m_white = 0;
    std::uint32_t m_black = 0;
    std::uint32_t m_kings = 0;
    Color m_side = Color::White;
    std::uint16_t m_quietPlies = 0;
    std::uint64_t m_key = 0;
};

// "c3-d4", "c3:e5:g3" in Russian notation; "22-18", "18x11x2" in English.
std::string ToNotation(const Move& move, Variant variant);
// The legal move that the squares name; the squares on the way may be left out where the move is clear without them.
std::optional<Move> ParseMove(std::string_view text, const Position& position, const Rules& rules);

enum class GameResult : std::uint8_t { Ongoing, WhiteWins, BlackWins, Draw };
enum class EndReason : std::uint8_t { None, NoPieces, NoMoves, Repetition, QuietMoves, LoneKings };

struct GameStatus {
    GameResult result = GameResult::Ongoing;
    EndReason reason = EndReason::None;

    constexpr bool Over() const { return result != GameResult::Ongoing; }
};

struct MoveRecord {
    Move move;
    bool king = false;            // the piece moved as a king
    std::uint32_t takenKings = 0; // the pieces taken that were kings
    std::string notation;
};

// A game: the variant, the starting position, the moves played, and the moves taken back that can be replayed.
class Game {
public:
    explicit Game(Variant variant = Variant::Russian) { Reset(variant, Position::Start(variant)); }

    void Reset(Variant variant, const Position& start);
    // A PDN game or position. The GameType tag chooses the variant; without one, the fallback is used.
    static std::expected<Game, std::string> Parse(std::string_view text, Variant fallback);
    // The variant, the starting position and the moves, for saving.
    std::string Serialize() const;
    static std::optional<Game> Deserialize(std::string_view text);

    Variant GetVariant() const { return m_variant; }
    const Rules& GetRules() const { return RulesOf(m_variant); }
    const Position& Current() const { return m_positions.back(); }
    const Position& Start() const { return m_positions.front(); }
    const Position& PositionAt(int ply) const { return m_positions[static_cast<std::size_t>(ply)]; }
    std::span<const MoveRecord> Moves() const { return m_moves; }
    int Ply() const { return static_cast<int>(m_moves.size()); }
    GameStatus Status() const { return m_status; }
    std::vector<Move> LegalMoves() const;

    bool Play(const Move& move); // only legal moves are played
    bool CanUndo() const { return !m_moves.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    bool Undo();
    bool Redo();
    const MoveRecord* NextRedo() const { return m_redo.empty() ? nullptr : &m_redo.back(); }

    // The keys of the earlier positions that a repetition could still return to, for the engine.
    std::vector<std::uint64_t> RepetitionHistory() const;

    std::string ToPdn(std::string_view whiteName, std::string_view blackName) const;

private:
    void Push(const Move& move);
    void UpdateStatus();

    Variant m_variant = Variant::Russian;
    std::vector<Position> m_positions;
    std::vector<MoveRecord> m_moves;
    std::vector<MoveRecord> m_redo;
    GameStatus m_status;
};

} // namespace draughts
