#include "Draughts.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <utility>

namespace draughts {

namespace {

constexpr std::array<Rules, VariantCount> AllRules{{
    // Russian draughts: a draw after 15 moves each made with kings only, taking nothing.
    {.flyingKings = true, .menTakeBack = true, .crownedTakesOn = true, .first = Color::White, .giveaway = false, .loneKingsDraw = true, .quietLimit = 30},
    // English draughts: Black moves first; a draw after 40 moves each without a capture or a man's move.
    {.flyingKings = false, .menTakeBack = false, .crownedTakesOn = false, .first = Color::Black, .giveaway = false, .loneKingsDraw = true, .quietLimit = 80},
    // Giveaway, on the Russian rules: whoever runs out of moves first wins.
    {.flyingKings = true, .menTakeBack = true, .crownedTakesOn = true, .first = Color::White, .giveaway = true, .loneKingsDraw = false, .quietLimit = 30},
}};

// The four diagonals: 0 and 1 lead towards Black (White's forward), 2 and 3 towards White.
constexpr std::array<int, 4> FileStep{1, -1, 1, -1};
constexpr std::array<int, 4> RankStep{1, 1, -1, -1};

constexpr bool IsForward(Color side, int direction) { return side == Color::White ? direction < 2 : direction >= 2; }
constexpr std::uint32_t CrownRow(Color side) { return side == Color::White ? 0xF0000000u : 0x0000000Fu; }

// The squares along each diagonal from each square, nearest first.
struct RayTable {
    std::array<std::array<std::array<std::uint8_t, 7>, 4>, SquareCount> squares{};
    std::array<std::array<std::uint8_t, 4>, SquareCount> length{};
};

constexpr RayTable BuildRays()
{
    RayTable table{};
    for (Square square = 0; square < SquareCount; ++square)
        for (int d = 0; d < 4; ++d)
            for (int file = FileOf(square) + FileStep[d], rank = RankOf(square) + RankStep[d]; IsDark(file, rank); file += FileStep[d], rank += RankStep[d])
                table.squares[square][d][table.length[square][d]++] = static_cast<std::uint8_t>(SquareAt(file, rank));
    return table;
}

constexpr RayTable Rays = BuildRays();

constexpr std::uint64_t SplitMix64(std::uint64_t& state)
{
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct ZobristKeys {
    std::array<std::array<std::uint64_t, SquareCount>, 4> piece{}; // white man, white king, black man, black king
    std::uint64_t side = 0;
};

constexpr ZobristKeys Zobrist = [] {
    ZobristKeys keys{};
    std::uint64_t state = 0x5D2C7A11E9F3B046ull;
    for (auto& squares : keys.piece)
        for (auto& key : squares)
            key = SplitMix64(state);
    keys.side = SplitMix64(state);
    return keys;
}();

constexpr int PieceKind(Color color, bool king) { return (color == Color::White ? 0 : 2) + (king ? 1 : 0); }

// Whether a king at a square could take something, in the middle of a capture that has taken `taken`.
bool FlyingKingCanTake(Square at, std::uint32_t enemy, std::uint32_t occupied, std::uint32_t taken)
{
    for (int d = 0; d < 4; ++d) {
        const auto& ray = Rays.squares[at][d];
        const int length = Rays.length[at][d];
        int k = 0;
        while (k < length && !(occupied & Bit(ray[k])))
            ++k;
        if (k + 1 < length && (enemy & Bit(ray[k])) && !(taken & Bit(ray[k])) && !(occupied & Bit(ray[k + 1])))
            return true;
    }
    return false;
}

// Builds every capture of one piece, jump by jump, by the rules: a piece cannot be jumped twice and stays on
// the board until the capture is over (the "Turkish strike"), a capture goes on while anything can be
// taken, and a flying king that can go on taking must land where it can.
class CaptureBuilder {
public:
    CaptureBuilder(const Rules& rules, Color side, std::uint32_t enemy, std::uint32_t occupied, MoveList& moves)
        : m_rules(rules), m_side(side), m_enemy(enemy), m_occupied(occupied), m_moves(moves)
    {
    }

    // `occupied` must leave out the piece that moves.
    void From(Square square, bool king)
    {
        m_move = Move{};
        m_move.from = static_cast<std::uint8_t>(square);
        m_crowned = false;
        m_first = m_moves.Size();
        if (king && m_rules.flyingKings)
            FlyingKing(square, 0);
        else
            ShortJumps(square, 0, king);
    }

private:
    void Push(Square landing, Square victim)
    {
        m_move.path[m_move.steps] = static_cast<std::uint8_t>(landing);
        m_move.taken[m_move.steps] = static_cast<std::uint8_t>(victim);
        ++m_move.steps;
    }

    void Pop() { --m_move.steps; }

    // A man, or a king that is not a flying one, jumping over a neighbour.
    void ShortJumps(Square at, std::uint32_t taken, bool king)
    {
        bool found = false;
        for (int d = 0; d < 4; ++d) {
            if ((!king && !m_rules.menTakeBack && !IsForward(m_side, d)) || Rays.length[at][d] < 2)
                continue;
            const Square victim = Rays.squares[at][d][0], landing = Rays.squares[at][d][1];
            if (!(m_enemy & Bit(victim)) || (taken & Bit(victim)) || (m_occupied & Bit(landing)))
                continue;
            found = true;
            const std::uint32_t now = taken | Bit(victim);
            Push(landing, victim);
            if (!king && (Bit(landing) & CrownRow(m_side))) {
                m_crowned = true;
                if (!m_rules.crownedTakesOn)
                    Record(now); // the crowning ends the move
                else if (m_rules.flyingKings)
                    FlyingKing(landing, now);
                else
                    ShortJumps(landing, now, true);
                m_crowned = false;
            }
            else {
                ShortJumps(landing, now, king);
            }
            Pop();
        }
        if (!found && m_move.steps > 0)
            Record(taken);
    }

    void FlyingKing(Square at, std::uint32_t taken)
    {
        bool found = false;
        for (int d = 0; d < 4; ++d) {
            const auto& ray = Rays.squares[at][d];
            const int length = Rays.length[at][d];
            int k = 0;
            while (k < length && !(m_occupied & Bit(ray[k])))
                ++k;
            if (k + 1 >= length || !(m_enemy & Bit(ray[k])) || (taken & Bit(ray[k])))
                continue;
            int end = k + 1;
            while (end < length && !(m_occupied & Bit(ray[end])))
                ++end;
            if (end == k + 1)
                continue;
            found = true;
            const Square victim = ray[k];
            const std::uint32_t now = taken | Bit(victim);
            // Where the king could go on taking, it must land on one of those squares.
            std::array<bool, 7> onward{};
            bool anyOnward = false;
            for (int i = k + 1; i < end; ++i) {
                onward[i] = FlyingKingCanTake(ray[i], m_enemy, m_occupied, now);
                anyOnward = anyOnward || onward[i];
            }
            for (int i = k + 1; i < end; ++i) {
                if (anyOnward && !onward[i])
                    continue;
                Push(ray[i], victim);
                FlyingKing(ray[i], now);
                Pop();
            }
        }
        if (!found && m_move.steps > 0)
            Record(taken);
    }

    void Record(std::uint32_t taken)
    {
        Move move = m_move;
        move.captured = taken;
        move.crowns = m_crowned;
        for (int i = m_first; i < m_moves.Size(); ++i)
            if (m_moves[i] == move)
                return; // another route to the same result
        m_moves.Add(move);
    }

    const Rules& m_rules;
    Color m_side;
    std::uint32_t m_enemy;
    std::uint32_t m_occupied;
    MoveList& m_moves;
    Move m_move;
    bool m_crowned = false;
    int m_first = 0;
};

std::string Lower(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::optional<int> ParseNumber(std::string_view text)
{
    if (text.empty() || text.size() > 3)
        return std::nullopt;
    int value = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

constexpr std::array<std::string_view, VariantCount> VariantNames{"russian", "english", "giveaway"};

} // namespace

const Rules& RulesOf(Variant variant) { return AllRules[std::to_underlying(variant)]; }

std::string SquareName(Square square, Variant variant)
{
    if (variant == Variant::English)
        return std::to_string((7 - RankOf(square)) * 4 + (square & 3) + 1);
    return {static_cast<char>('a' + FileOf(square)), static_cast<char>('1' + RankOf(square))};
}

std::optional<Square> ParseSquare(std::string_view text)
{
    if (const auto number = ParseNumber(text)) {
        if (*number < 1 || *number > 32)
            return std::nullopt;
        const int rank = 7 - (*number - 1) / 4;
        return rank * 4 + (*number - 1) % 4;
    }
    if (text.size() == 2) {
        const int file = std::tolower(static_cast<unsigned char>(text[0])) - 'a', rank = text[1] - '1';
        if (const Square square = SquareAt(file, rank); square >= 0)
            return square;
    }
    return std::nullopt;
}

// Positions ------------------------------------------------------------------------------------------

Position Position::Start(Variant variant)
{
    Position position;
    position.m_white = 0x00000FFFu; // the first three ranks
    position.m_black = 0xFFF00000u; // the last three
    position.m_side = RulesOf(variant).first;
    position.ComputeKey();
    return position;
}

std::expected<Position, std::string> Position::FromFen(std::string_view fen)
{
    std::string text;
    for (char c : fen)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"')
            text += c;
    while (!text.empty() && text.back() == '.')
        text.pop_back();

    std::vector<std::string_view> parts;
    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t colon = std::min(text.find(':', start), text.size());
        parts.emplace_back(std::string_view(text).substr(start, colon - start));
        start = colon + 1;
    }
    if (parts.size() < 2 || parts[0].size() != 1)
        return std::unexpected("The position should start with the side to move, W or B.");

    Position position;
    const char turn = static_cast<char>(std::toupper(static_cast<unsigned char>(parts[0][0])));
    if (turn != 'W' && turn != 'B')
        return std::unexpected("The side to move should be W or B.");
    position.m_side = turn == 'W' ? Color::White : Color::Black;

    for (std::size_t p = 1; p < parts.size(); ++p) {
        std::string_view part = parts[p];
        if (part.empty())
            continue;
        const char side = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
        if (side != 'W' && side != 'B')
            return std::unexpected(std::format("Expected W or B before the squares, found \"{}\".", part));
        std::uint32_t& pieces = side == 'W' ? position.m_white : position.m_black;
        part.remove_prefix(1);
        while (!part.empty()) {
            const std::size_t comma = std::min(part.find(','), part.size());
            std::string_view item = part.substr(0, comma);
            part.remove_prefix(std::min(comma + 1, part.size()));
            if (item.empty())
                continue;
            const bool king = item[0] == 'K' || item[0] == 'k';
            if (king)
                item.remove_prefix(1);
            // A square, or a range of numbered squares such as "1-12".
            std::vector<Square> squares;
            if (const std::size_t dash = item.find('-'); dash != std::string_view::npos) {
                const auto a = ParseNumber(item.substr(0, dash)), b = ParseNumber(item.substr(dash + 1));
                if (!a || !b || *a < 1 || *b > 32 || *a > *b)
                    return std::unexpected(std::format("\"{}\" is not a range of squares.", item));
                for (int number = *a; number <= *b; ++number)
                    squares.push_back(*ParseSquare(std::to_string(number)));
            }
            else if (const auto square = ParseSquare(item)) {
                squares.push_back(*square);
            }
            else {
                return std::unexpected(std::format("\"{}\" is not a dark square.", item));
            }
            for (const Square square : squares) {
                if ((position.Occupied() & Bit(square)) != 0)
                    return std::unexpected("A square is taken twice.");
                pieces |= Bit(square);
                if (king)
                    position.m_kings |= Bit(square);
            }
        }
    }
    if ((position.Men(Color::White) & CrownRow(Color::White)) || (position.Men(Color::Black) & CrownRow(Color::Black)))
        return std::unexpected("A man cannot stand on the far rank; it would be a king.");
    if (position.Occupied() == 0)
        return std::unexpected("The position has no pieces.");
    position.ComputeKey();
    return position;
}

std::string Position::ToFen(Variant variant) const
{
    std::string fen = m_side == Color::White ? "W" : "B";
    for (const Color color : {Color::White, Color::Black}) {
        fen += color == Color::White ? ":W" : ":B";
        std::vector<Square> squares;
        for (std::uint32_t pieces = Pieces(color); pieces; pieces &= pieces - 1)
            squares.push_back(std::countr_zero(pieces));
        if (variant == Variant::English) // by number
            std::ranges::sort(squares, [](Square a, Square b) { return (7 - RankOf(a)) * 4 + (a & 3) < (7 - RankOf(b)) * 4 + (b & 3); });
        bool first = true;
        for (const Square square : squares) {
            if (!first)
                fen += ',';
            first = false;
            if (m_kings & Bit(square))
                fen += 'K';
            fen += SquareName(square, variant);
        }
    }
    return fen;
}

std::optional<Piece> Position::At(Square square) const
{
    if (!(Occupied() & Bit(square)))
        return std::nullopt;
    return Piece{(m_white & Bit(square)) ? Color::White : Color::Black, (m_kings & Bit(square)) != 0};
}

bool Position::CanTake(const Rules& rules, Square square) const
{
    const Color side = (m_white & Bit(square)) ? Color::White : Color::Black;
    const std::uint32_t enemy = Pieces(Opposite(side)), occupied = Occupied();
    const bool king = (m_kings & Bit(square)) != 0;
    if (king && rules.flyingKings)
        return FlyingKingCanTake(square, enemy, occupied & ~Bit(square), 0);
    for (int d = 0; d < 4; ++d) {
        if ((!king && !rules.menTakeBack && !IsForward(side, d)) || Rays.length[square][d] < 2)
            continue;
        if ((enemy & Bit(Rays.squares[square][d][0])) && !(occupied & Bit(Rays.squares[square][d][1])))
            return true;
    }
    return false;
}

bool Position::HasCapture(const Rules& rules) const { return PiecesThatCanTake(rules) != 0; }

std::uint32_t Position::PiecesThatCanTake(const Rules& rules) const
{
    std::uint32_t result = 0;
    for (std::uint32_t pieces = Pieces(m_side); pieces; pieces &= pieces - 1) {
        const Square square = std::countr_zero(pieces);
        if (CanTake(rules, square))
            result |= Bit(square);
    }
    return result;
}

void Position::GenerateMoves(const Rules& rules, MoveList& moves) const
{
    moves.Clear();
    const std::uint32_t own = Pieces(m_side), enemy = Pieces(Opposite(m_side)), occupied = Occupied();

    // Taking is compulsory: when anything can be taken, the captures are the only moves.
    for (std::uint32_t pieces = own; pieces; pieces &= pieces - 1) {
        const Square square = std::countr_zero(pieces);
        if (CanTake(rules, square)) {
            CaptureBuilder builder(rules, m_side, enemy, occupied & ~Bit(square), moves);
            builder.From(square, (m_kings & Bit(square)) != 0);
        }
    }
    if (!moves.Empty())
        return;

    for (std::uint32_t pieces = own; pieces; pieces &= pieces - 1) {
        const Square square = std::countr_zero(pieces);
        const bool king = (m_kings & Bit(square)) != 0;
        for (int d = 0; d < 4; ++d) {
            if (!king && !IsForward(m_side, d))
                continue;
            const auto& ray = Rays.squares[square][d];
            const int reach = king && rules.flyingKings ? Rays.length[square][d] : std::min<int>(1, Rays.length[square][d]);
            for (int k = 0; k < reach && !(occupied & Bit(ray[k])); ++k) {
                Move move;
                move.from = static_cast<std::uint8_t>(square);
                move.steps = 1;
                move.path[0] = ray[k];
                move.crowns = !king && (Bit(ray[k]) & CrownRow(m_side)) != 0;
                moves.Add(move);
            }
        }
    }
}

Position Position::After(const Move& move) const
{
    Position next = *this;
    next.Apply(move);
    return next;
}

void Position::Apply(const Move& move)
{
    const Color side = m_side;
    std::uint32_t& own = side == Color::White ? m_white : m_black;
    std::uint32_t& enemy = side == Color::White ? m_black : m_white;
    const Square to = move.To();
    const bool king = (m_kings & Bit(move.from)) != 0;
    const bool crowned = king || move.crowns;

    m_key ^= Zobrist.piece[PieceKind(side, king)][move.from] ^ Zobrist.piece[PieceKind(side, crowned)][to];
    own = (own & ~Bit(move.from)) | Bit(to);
    m_kings &= ~Bit(move.from);
    if (crowned)
        m_kings |= Bit(to);
    for (std::uint32_t taken = move.captured; taken; taken &= taken - 1) {
        const Square square = std::countr_zero(taken);
        m_key ^= Zobrist.piece[PieceKind(Opposite(side), (m_kings & Bit(square)) != 0)][square];
    }
    enemy &= ~move.captured;
    m_kings &= ~move.captured;
    m_quietPlies = king && !move.IsCapture() ? static_cast<std::uint16_t>(m_quietPlies + 1) : std::uint16_t{0};
    m_side = Opposite(side);
    m_key ^= Zobrist.side;
}

void Position::ComputeKey()
{
    m_key = m_side == Color::Black ? Zobrist.side : 0;
    for (std::uint32_t pieces = Occupied(); pieces; pieces &= pieces - 1) {
        const Square square = std::countr_zero(pieces);
        const Color color = (m_white & Bit(square)) ? Color::White : Color::Black;
        m_key ^= Zobrist.piece[PieceKind(color, (m_kings & Bit(square)) != 0)][square];
    }
}

// Notation -------------------------------------------------------------------------------------------

std::string ToNotation(const Move& move, Variant variant)
{
    std::string text = SquareName(move.from, variant);
    const char separator = !move.IsCapture() ? '-' : variant == Variant::English ? 'x' : ':';
    for (int i = 0; i < move.steps; ++i) {
        text += separator;
        text += SquareName(move.path[i], variant);
    }
    return text;
}

std::optional<Move> ParseMove(std::string_view text, const Position& position, const Rules& rules)
{
    std::vector<Square> squares;
    std::string token;
    auto flush = [&] {
        if (!token.empty()) {
            if (const auto square = ParseSquare(token))
                squares.push_back(*square);
            else
                squares.push_back(-1);
            token.clear();
        }
    };
    for (char c : text) {
        if (c == '-' || c == ':' || c == 'x' || c == 'X')
            flush();
        else if (std::isalnum(static_cast<unsigned char>(c)))
            token += c;
        else
            flush(); // marks such as "!" or "?"
    }
    flush();
    if (squares.size() < 2 || std::ranges::contains(squares, -1))
        return std::nullopt;

    MoveList moves;
    position.GenerateMoves(rules, moves);
    std::optional<Move> found;
    for (const Move& move : moves) {
        if (move.from != squares.front() || move.To() != squares.back())
            continue;
        // The squares named on the way must be landings of the move, in order.
        std::size_t next = 1;
        for (int step = 0; step + 1 < move.steps && next + 1 < squares.size(); ++step)
            if (move.path[step] == squares[next])
                ++next;
        if (next + 1 != squares.size())
            continue;
        if (static_cast<std::size_t>(move.steps) + 1 == squares.size())
            return move; // the whole route was given
        if (!found)
            found = move;
    }
    return found;
}

// The game -------------------------------------------------------------------------------------------

void Game::Reset(Variant variant, const Position& start)
{
    m_variant = variant;
    m_positions.assign(1, start);
    m_moves.clear();
    m_redo.clear();
    UpdateStatus();
}

std::vector<Move> Game::LegalMoves() const
{
    if (m_status.Over())
        return {};
    MoveList moves;
    Current().GenerateMoves(GetRules(), moves);
    return {moves.begin(), moves.end()};
}

void Game::Push(const Move& move)
{
    const Position& position = Current();
    MoveRecord record;
    record.move = move;
    record.king = (position.Kings() & Bit(move.from)) != 0;
    record.takenKings = move.captured & position.Kings();
    record.notation = ToNotation(move, m_variant);
    m_moves.push_back(std::move(record));
    m_positions.push_back(position.After(move));
}

bool Game::Play(const Move& move)
{
    const auto legal = LegalMoves();
    const auto it = std::ranges::find(legal, move);
    if (it == legal.end())
        return false;
    Push(*it);
    m_redo.clear();
    UpdateStatus();
    return true;
}

bool Game::Undo()
{
    if (m_moves.empty())
        return false;
    m_redo.push_back(std::move(m_moves.back()));
    m_moves.pop_back();
    m_positions.pop_back();
    UpdateStatus();
    return true;
}

bool Game::Redo()
{
    if (m_redo.empty())
        return false;
    const Move move = m_redo.back().move;
    m_redo.pop_back();
    Push(move);
    UpdateStatus();
    return true;
}

std::vector<std::uint64_t> Game::RepetitionHistory() const
{
    // Moving a man or taking a piece cannot be undone, so no position before it can come back.
    std::size_t first = m_moves.size();
    while (first > 0 && m_moves[first - 1].king && !m_moves[first - 1].move.IsCapture())
        --first;
    std::vector<std::uint64_t> keys;
    for (std::size_t i = first; i < m_moves.size(); ++i)
        keys.push_back(m_positions[i].Key());
    return keys;
}

void Game::UpdateStatus()
{
    m_status = {};
    const Position& position = Current();
    const Rules& rules = GetRules();
    MoveList moves;
    position.GenerateMoves(rules, moves);
    const Color side = position.SideToMove();
    if (moves.Empty()) {
        const bool sideWins = rules.giveaway;
        const Color winner = sideWins ? side : Opposite(side);
        m_status.result = winner == Color::White ? GameResult::WhiteWins : GameResult::BlackWins;
        m_status.reason = position.Pieces(side) == 0 ? EndReason::NoPieces : EndReason::NoMoves;
        return;
    }
    if (position.QuietPlies() >= rules.quietLimit) {
        m_status = {GameResult::Draw, EndReason::QuietMoves};
        return;
    }
    const auto history = RepetitionHistory();
    if (std::ranges::count(history, position.Key()) >= 2) {
        m_status = {GameResult::Draw, EndReason::Repetition};
        return;
    }
    if (rules.loneKingsDraw && std::popcount(position.Pieces(Color::White)) == 1 && std::popcount(position.Pieces(Color::Black)) == 1 &&
        (position.Kings() & position.Occupied()) == position.Occupied() && !moves[0].IsCapture())
        m_status = {GameResult::Draw, EndReason::LoneKings};
}

std::expected<Game, std::string> Game::Parse(std::string_view text, Variant fallback)
{
    // The tags, and the moves with comments and variations left out.
    Variant variant = fallback;
    std::optional<Position> start;
    std::string moves;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (depth == 0 && c == '[') {
            const std::size_t end = text.find(']', i);
            if (end == std::string_view::npos)
                break;
            const std::string_view tag = text.substr(i + 1, end - i - 1);
            const std::size_t quote = tag.find('"');
            const std::string name = Lower(Trim(tag.substr(0, quote)));
            std::string_view value;
            if (quote != std::string_view::npos) {
                value = tag.substr(quote + 1);
                value = value.substr(0, value.find('"'));
            }
            if (name == "gametype") {
                if (value.starts_with("21"))
                    variant = Variant::English;
                else if (value.starts_with("25"))
                    variant = variant == Variant::Giveaway ? Variant::Giveaway : Variant::Russian;
            }
            else if (name == "variant" && (Lower(value).find("giveaway") != std::string::npos || value.find("оддавк") != std::string_view::npos)) {
                variant = Variant::Giveaway;
            }
            else if (name == "fen") {
                auto position = Position::FromFen(value);
                if (!position)
                    return std::unexpected(position.error());
                start = *position;
            }
            i = end;
            continue;
        }
        if (c == '{' || c == '(')
            ++depth;
        else if ((c == '}' || c == ')') && depth > 0)
            --depth;
        else if (depth == 0)
            moves += c;
    }

    // A position on its own.
    const std::string_view rest = Trim(moves);
    if (!start && rest.size() > 2 && (rest[0] == 'W' || rest[0] == 'B' || rest[0] == 'w' || rest[0] == 'b') && rest[1] == ':') {
        auto position = Position::FromFen(rest);
        if (!position)
            return std::unexpected(position.error());
        Game game(variant);
        game.Reset(variant, *position);
        return game;
    }

    Game game(variant);
    game.Reset(variant, start ? *start : Position::Start(variant));
    std::size_t played = 0;
    std::size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i])))
            ++i;
        std::size_t end = i;
        while (end < rest.size() && !std::isspace(static_cast<unsigned char>(rest[end])))
            ++end;
        std::string_view token = rest.substr(i, end - i);
        i = end;
        if (token.empty())
            continue;
        if (const std::size_t dot = token.rfind('.'); dot != std::string_view::npos)
            token.remove_prefix(dot + 1); // a move number
        if (token.empty())
            continue;
        if (token == "*" || token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "2-0" || token == "0-2" || token == "1-1" || token == "0-0")
            break;
        const auto move = ParseMove(token, game.Current(), game.GetRules());
        if (!move || !game.Play(*move))
            return std::unexpected(std::format("The move \"{}\" cannot be played.", token));
        ++played;
    }
    if (played == 0 && !start)
        return std::unexpected("There is no game or position here.");
    return game;
}

std::string Game::Serialize() const
{
    std::string text = std::format("{}|{}|", VariantNames[std::to_underlying(m_variant)], Start().ToFen(Variant::Russian));
    for (std::size_t i = 0; i < m_moves.size(); ++i) {
        if (i > 0)
            text += ' ';
        text += ToNotation(m_moves[i].move, Variant::Russian);
    }
    return text;
}

std::optional<Game> Game::Deserialize(std::string_view text)
{
    const std::size_t first = text.find('|');
    const std::size_t second = first == std::string_view::npos ? first : text.find('|', first + 1);
    if (second == std::string_view::npos)
        return std::nullopt;
    const auto name = std::ranges::find(VariantNames, text.substr(0, first));
    if (name == VariantNames.end())
        return std::nullopt;
    const auto variant = static_cast<Variant>(name - VariantNames.begin());
    auto start = Position::FromFen(text.substr(first + 1, second - first - 1));
    if (!start)
        return std::nullopt;
    Game game(variant);
    game.Reset(variant, *start);
    std::string_view moves = text.substr(second + 1);
    while (!moves.empty()) {
        const std::size_t space = std::min(moves.find(' '), moves.size());
        const std::string_view token = moves.substr(0, space);
        moves.remove_prefix(std::min(space + 1, moves.size()));
        if (token.empty())
            continue;
        const auto move = ParseMove(token, game.Current(), game.GetRules());
        if (!move || !game.Play(*move))
            break; // keep what could be played
    }
    return game;
}

std::string Game::ToPdn(std::string_view whiteName, std::string_view blackName) const
{
    const auto today = std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    const char* result = m_status.result == GameResult::WhiteWins ? "1-0"
                         : m_status.result == GameResult::BlackWins ? "0-1"
                         : m_status.result == GameResult::Draw      ? "1/2-1/2"
                                                                     : "*";
    std::string pdn = std::format("[Event \"3D Checkers\"]\n[Date \"{:04}.{:02}.{:02}\"]\n[White \"{}\"]\n[Black \"{}\"]\n[Result \"{}\"]\n",
                                  static_cast<int>(today.year()), static_cast<unsigned>(today.month()), static_cast<unsigned>(today.day()), whiteName,
                                  blackName, result);
    pdn += std::format("[GameType \"{}\"]\n", m_variant == Variant::English ? 21 : 25);
    if (m_variant == Variant::Giveaway)
        pdn += "[Variant \"Giveaway\"]\n";
    if (Start().Key() != Position::Start(m_variant).Key())
        pdn += std::format("[FEN \"{}\"]\n", Start().ToFen(m_variant));
    pdn += '\n';

    // The moves in pairs, the side that moves first in the variant first in each pair.
    std::string line;
    auto add = [&](const std::string& word) {
        if (!line.empty() && line.size() + word.size() + 1 > 80) {
            pdn += line + '\n';
            line.clear();
        }
        if (!line.empty())
            line += ' ';
        line += word;
    };
    const Color first = GetRules().first;
    int number = 1;
    for (std::size_t i = 0; i < m_moves.size(); ++i) {
        const Color mover = m_positions[i].SideToMove();
        if (mover == first)
            add(std::format("{}. {}", number, m_moves[i].notation));
        else if (i == 0)
            add(std::format("{}... {}", number, m_moves[i].notation));
        else
            add(m_moves[i].notation);
        if (mover != first)
            ++number;
    }
    add(result);
    pdn += line + '\n';
    return pdn;
}

} // namespace draughts
