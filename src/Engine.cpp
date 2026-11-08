#include "Engine.h"

#include <algorithm>
#include <bit>
#include <random>
#include <utility>

namespace draughts {

namespace {

constexpr int Infinity = 32000;

enum Bound : std::uint8_t { NoBound, UpperBound, LowerBound, ExactBound };

// The value of a man by how far it has come (its rank counted from its own side), and of standing on a square.
constexpr std::array<int, 8> Advance{0, 2, 4, 7, 11, 16, 24, 0};
// For White; Black's squares are mirrored. The middle of the board is worth holding, the edges less.
constexpr std::array<int, 32> ManSquare{
    0, 0, 0, 0,  // a1 c1 e1 g1
    0, 2, 2, -2, // b2 d2 f2 h2
    -3, 4, 4, 1, // a3 c3 e3 g3
    1, 7, 6, -3, // b4 d4 f4 h4
    -3, 7, 7, 1, // a5 c5 e5 g5
    1, 5, 5, -2, // b6 d6 f6 h6
    -2, 3, 3, 1, // a7 c7 e7 g7
    0, 0, 0, 0,  // b8 d8 f8 h8
};

constexpr std::uint32_t BackRank(Color color) { return color == Color::White ? 0x0000000Fu : 0xF0000000u; }

// The long diagonal a1-h8 and the double diagonals beside it, where a flying king is strongest.
constexpr std::uint32_t MainDiagonal = [] {
    std::uint32_t mask = 0;
    for (int i = 0; i < 8; ++i)
        mask |= Bit(SquareAt(i, i));
    return mask;
}();

constexpr std::uint32_t DoubleDiagonals = [] {
    std::uint32_t mask = 0;
    for (int i = 0; i < 7; ++i)
        mask |= Bit(SquareAt(6 - i, i)) | Bit(SquareAt(7 - i, i + 1)); // g1-a7 and h2-b8
    return mask;
}();

constexpr Square Mirror(Square square) { return 31 - square; }

int KingMobility(Square square, std::uint32_t occupied, bool flying)
{
    constexpr std::array<int, 4> fileStep{1, -1, 1, -1}, rankStep{1, 1, -1, -1};
    int count = 0;
    for (int d = 0; d < 4; ++d) {
        for (int file = FileOf(square) + fileStep[d], rank = RankOf(square) + rankStep[d]; IsDark(file, rank);
             file += fileStep[d], rank += rankStep[d]) {
            if (occupied & Bit(SquareAt(file, rank)))
                break;
            ++count;
            if (!flying)
                break;
        }
    }
    return count;
}

int Side(const Position& position, Color color, Variant variant)
{
    const bool russian = variant != Variant::English;
    const int manValue = 100, kingValue = russian ? 280 : 145;
    const std::uint32_t men = position.Men(color), kings = position.Kings(color);
    const std::uint32_t enemyKings = position.Kings(Opposite(color));
    const std::uint32_t occupied = position.Occupied();
    int score = std::popcount(men) * manValue + std::popcount(kings) * kingValue;

    for (std::uint32_t bits = men; bits; bits &= bits - 1) {
        const Square square = std::countr_zero(bits);
        const Square own = color == Color::White ? square : Mirror(square);
        score += Advance[RankOf(own)] + ManSquare[own];
    }
    // Men at home keep the other side from crowning, while it has no kings to come round with.
    if (enemyKings == 0)
        score += std::popcount(men & BackRank(color)) * 6;

    for (std::uint32_t bits = kings; bits; bits &= bits - 1) {
        const Square square = std::countr_zero(bits);
        if (russian) {
            if (MainDiagonal & Bit(square))
                score += 20;
            else if (DoubleDiagonals & Bit(square))
                score += 6;
            score += 2 * KingMobility(square, occupied, true);
        }
        else {
            score += 4 * KingMobility(square, occupied, false) + (ManSquare[square] + ManSquare[Mirror(square)]) / 2;
        }
    }
    return score;
}

std::uint16_t Code(const Move& move)
{
    return static_cast<std::uint16_t>(0x8000 | move.from | (move.To() << 5) | (std::min(move.CaptureCount(), 15) << 10));
}

} // namespace

Engine::Engine(int hashMegabytes)
{
    std::size_t entries = 1;
    while (entries * 2 * sizeof(Entry) <= static_cast<std::size_t>(hashMegabytes) * 1024 * 1024)
        entries *= 2;
    m_table.resize(entries);
    m_lists.resize(MaxPly + 1);
}

void Engine::Clear()
{
    std::ranges::fill(m_table, Entry{});
    m_killers = {};
    m_history = {};
}

int Engine::Evaluate(const Position& position, Variant variant)
{
    const Color side = position.SideToMove();
    if (variant == Variant::Giveaway) {
        // Fewer pieces is better; men pressing forward give the other side something to take.
        auto pieces = [&](Color color) {
            int score = std::popcount(position.Pieces(color)) * 100 + std::popcount(position.Kings(color)) * 40;
            for (std::uint32_t men = position.Men(color); men; men &= men - 1) {
                const Square square = std::countr_zero(men);
                score -= 3 * RankOf(color == Color::White ? square : Mirror(square));
            }
            return score;
        };
        return pieces(Opposite(side)) - pieces(side);
    }

    const int own = Side(position, side, variant), other = Side(position, Opposite(side), variant);
    int score = own - other;
    // When ahead, trading down brings the win closer.
    const int pieces = std::popcount(position.Occupied());
    score += (own - other) * (24 - pieces) / 40;
    return score;
}

bool Engine::ShouldStop() { return m_stop.stop_requested() || std::chrono::steady_clock::now() >= m_deadline; }

bool Engine::IsRepetition(const Position& position) const
{
    // Only king moves without captures can come back to a position, and only as far back as they reach.
    const int last = static_cast<int>(m_keys.size()) - 1;
    for (int i = last - 2; i >= 0 && i >= last - position.QuietPlies(); i -= 2)
        if (m_keys[static_cast<std::size_t>(i)] == position.Key())
            return true;
    return false;
}

int Engine::ScoreMove(const Position& position, const Move& move, int ply) const
{
    if (move.IsCapture())
        return 100000 + 1000 * move.CaptureCount() + 400 * std::popcount(move.captured & position.Kings()) + (move.crowns ? 300 : 0);
    if (move.crowns)
        return 90000;
    const std::uint16_t code = Code(move);
    if (code == m_killers[ply][0])
        return 80000;
    if (code == m_killers[ply][1])
        return 79000;
    return std::min(m_history[move.from][move.To()], 70000);
}

void Engine::Store(std::uint64_t key, int depth, int score, int bound, int move, int ply)
{
    if (score >= Win - MaxPly)
        score += ply;
    else if (score <= -Win + MaxPly)
        score -= ply;
    Entry& entry = m_table[key & (m_table.size() - 1)];
    if (entry.key == key && entry.depth > depth && bound != ExactBound)
        return;
    entry.key = key;
    entry.score = static_cast<std::int16_t>(score);
    entry.depth = static_cast<std::int8_t>(std::clamp(depth, -100, 100));
    entry.bound = static_cast<std::uint8_t>(bound);
    entry.move = static_cast<std::uint16_t>(move);
}

int Engine::Negamax(const Position& position, int depth, int alpha, int beta, int ply)
{
    if ((++m_nodes & 2047) == 0 && ShouldStop())
        m_aborted = true;
    if (m_aborted)
        return 0;

    const Rules& rules = *m_rules;
    if (ply > 0 && (position.QuietPlies() >= rules.quietLimit || IsRepetition(position)))
        return 0;

    MoveList& moves = m_lists[static_cast<std::size_t>(ply)];
    position.GenerateMoves(rules, moves);
    if (moves.Empty())
        return rules.giveaway ? Win - ply : -(Win - ply);
    const bool capture = moves[0].IsCapture();
    if (rules.loneKingsDraw && !capture && std::popcount(position.Pieces(Color::White)) == 1 &&
        std::popcount(position.Pieces(Color::Black)) == 1 && (position.Kings() & position.Occupied()) == position.Occupied())
        return 0;
    // A position with something to take is never left to the evaluation: the captures are followed to the end.
    if ((depth <= 0 && !capture) || ply >= MaxPly - 1)
        return Evaluate(position, m_variant);

    const std::uint64_t key = position.Key();
    const Entry& entry = m_table[key & (m_table.size() - 1)];
    std::uint16_t ttMove = 0;
    if (entry.key == key) {
        ttMove = entry.move;
        if (ply > 0 && entry.depth >= depth) {
            int score = entry.score;
            if (score >= Win - MaxPly)
                score -= ply;
            else if (score <= -Win + MaxPly)
                score += ply;
            if (entry.bound == ExactBound || (entry.bound == LowerBound && score >= beta) || (entry.bound == UpperBound && score <= alpha))
                return score;
        }
    }

    // Best first: the move that was best here before, then captures, crownings, killers and history.
    std::array<int, MoveList::Capacity> scores{};
    for (int i = 0; i < moves.Size(); ++i)
        scores[static_cast<std::size_t>(i)] = Code(moves[i]) == ttMove ? 1000000 : ScoreMove(position, moves[i], ply);
    for (int i = 1; i < moves.Size(); ++i)
        for (int j = i; j > 0 && scores[static_cast<std::size_t>(j)] > scores[static_cast<std::size_t>(j - 1)]; --j) {
            std::swap(scores[static_cast<std::size_t>(j)], scores[static_cast<std::size_t>(j - 1)]);
            std::swap(moves[j], moves[j - 1]);
        }

    // A forced move costs nothing: it is searched as deep as the position before it.
    const bool forced = moves.Size() == 1 && ply < 2 * m_rootDepth + 8;
    const int alphaBefore = alpha;
    int best = -Infinity;
    std::uint16_t bestCode = 0;
    for (int i = 0; i < moves.Size(); ++i) {
        const Move move = moves[i];
        const Position next = position.After(move);
        int newDepth = depth - (forced ? 0 : 1);
        m_keys.push_back(next.Key());
        int score;
        if (i == 0) {
            score = -Negamax(next, newDepth, -beta, -alpha, ply + 1);
        }
        else {
            // Late quiet moves are first searched a little less deeply.
            const int reduction = !capture && !move.crowns && i >= 3 && depth >= 3 ? 1 : 0;
            score = -Negamax(next, newDepth - reduction, -alpha - 1, -alpha, ply + 1);
            if (score > alpha && (reduction > 0 || score < beta))
                score = -Negamax(next, newDepth, -beta, -alpha, ply + 1);
        }
        m_keys.pop_back();
        if (m_aborted)
            return 0;

        if (score > best) {
            best = score;
            bestCode = Code(move);
        }
        if (score > alpha) {
            alpha = score;
            if (alpha >= beta) {
                if (!move.IsCapture()) {
                    auto& killers = m_killers[static_cast<std::size_t>(ply)];
                    if (killers[0] != bestCode) {
                        killers[1] = killers[0];
                        killers[0] = bestCode;
                    }
                    m_history[move.from][move.To()] += depth * depth;
                }
                break;
            }
        }
    }
    Store(key, depth, best, best >= beta ? LowerBound : best > alphaBefore ? ExactBound : UpperBound, bestCode, ply);
    return best;
}

SearchResult Engine::Search(const Position& root, Variant variant, std::span<const std::uint64_t> history, const SearchLimits& limits,
                            std::stop_token stop)
{
    const auto start = std::chrono::steady_clock::now();
    m_deadline = start + std::chrono::milliseconds(limits.milliseconds);
    m_stop = stop;
    m_aborted = false;
    m_nodes = 0;
    m_variant = variant;
    m_rules = &RulesOf(variant);
    m_keys.assign(history.begin(), history.end());
    m_keys.push_back(root.Key());
    m_killers = {};
    for (auto& row : m_history)
        for (int& value : row)
            value /= 8;

    struct RootMove {
        Move move;
        int score = -Infinity;
    };
    MoveList generated;
    root.GenerateMoves(*m_rules, generated);
    if (generated.Empty())
        return {};
    std::vector<RootMove> rootMoves;
    for (const Move& move : generated)
        rootMoves.push_back({move});
    std::ranges::stable_sort(rootMoves, [&](const RootMove& a, const RootMove& b) { return ScoreMove(root, a.move, 0) > ScoreMove(root, b.move, 0); });

    SearchResult result{rootMoves.front().move, 0, 0, 0};
    if (rootMoves.size() == 1 && limits.randomMargin == 0)
        return result;

    const bool exactScores = limits.randomMargin > 0;
    for (int depth = 1; depth <= limits.maxDepth; ++depth) {
        m_rootDepth = depth;
        int alpha = -Infinity;
        const int beta = Infinity;
        Move iterationBest;
        int iterationScore = -Infinity;
        bool complete = true;

        for (std::size_t i = 0; i < rootMoves.size(); ++i) {
            RootMove& rootMove = rootMoves[i];
            const Position next = root.After(rootMove.move);
            m_keys.push_back(next.Key());
            int score;
            if (i == 0 || exactScores) {
                score = -Negamax(next, depth - 1, -beta, exactScores ? Infinity : -alpha, 1);
            }
            else {
                score = -Negamax(next, depth - 1, -alpha - 1, -alpha, 1);
                if (!m_aborted && score > alpha)
                    score = -Negamax(next, depth - 1, -beta, -alpha, 1);
            }
            m_keys.pop_back();
            if (m_aborted) {
                complete = false;
                break;
            }
            rootMove.score = score;
            if (score > iterationScore) {
                iterationScore = score;
                iterationBest = rootMove.move;
            }
            alpha = std::max(alpha, score);
        }

        // An unfinished iteration still counts if a move beat the previous best, which it searched first.
        if (!iterationBest.IsNull() && (complete || iterationScore > result.score || result.depth == 0)) {
            result.best = iterationBest;
            result.score = iterationScore;
        }
        if (!complete)
            break;
        result.depth = depth;

        std::ranges::stable_sort(rootMoves, [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::milliseconds(limits.milliseconds) / 2 || IsWinScore(iterationScore))
            break;
    }

    if (exactScores && result.depth > 0) {
        // The weaker levels pick among the good moves, the better ones more often.
        std::mt19937_64 random(limits.seed ^ root.Key());
        std::vector<RootMove> candidates;
        for (const RootMove& rootMove : rootMoves)
            if (rootMove.score >= rootMoves.front().score - limits.randomMargin)
                candidates.push_back(rootMove);
        std::vector<double> weights;
        for (const RootMove& candidate : candidates)
            weights.push_back(1.0 + static_cast<double>(limits.randomMargin - (rootMoves.front().score - candidate.score)) /
                                        std::max(1, limits.randomMargin) * 3.0);
        std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());
        const RootMove& chosen = candidates[pick(random)];
        result.best = chosen.move;
        result.score = chosen.score;
    }
    result.nodes = m_nodes;
    return result;
}

// AiPlayer -----------------------------------------------------------------------------------------

void AiPlayer::Start(const Position& position, Variant variant, std::vector<std::uint64_t> history, const SearchLimits& limits)
{
    Cancel();
    m_busy = true;
    m_worker = std::jthread([this, position, variant, history = std::move(history), limits](std::stop_token stop) {
        SearchResult result = m_engine.Search(position, variant, history, limits, stop);
        if (!stop.stop_requested()) {
            std::scoped_lock lock(m_mutex);
            m_result = result;
        }
        m_busy = false;
    });
}

void AiPlayer::Cancel()
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_busy = false;
    std::scoped_lock lock(m_mutex);
    m_result.reset();
}

void AiPlayer::NewGame()
{
    Cancel();
    m_engine.Clear();
}

std::optional<SearchResult> AiPlayer::TakeResult()
{
    std::scoped_lock lock(m_mutex);
    return std::exchange(m_result, std::nullopt);
}

} // namespace draughts
