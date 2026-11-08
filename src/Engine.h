#pragma once

#include "Draughts.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace draughts {

struct SearchLimits {
    int maxDepth = 64;
    int milliseconds = 1000; // iterative deepening stops when this runs out
    int randomMargin = 0;    // a random move among those this close to the best, for the weaker levels
    std::uint64_t seed = 0;
};

struct SearchResult {
    Move best;
    int score = 0; // for the side to move, a man being 100
    int depth = 0;
    std::uint64_t nodes = 0;
};

// An alpha-beta searcher: iterative deepening, principal variation search, a transposition table, and
// captures followed to the end, since a position with something to take is never quiet.
class Engine {
public:
    explicit Engine(int hashMegabytes = 32);

    SearchResult Search(const Position& root, Variant variant, std::span<const std::uint64_t> history, const SearchLimits& limits,
                        std::stop_token stop);
    void Clear();

    // Material and position for the side to move.
    static int Evaluate(const Position& position, Variant variant);

    static constexpr int Win = 30000;
    static bool IsWinScore(int score) { return score >= Win - 512 || score <= -Win + 512; }

private:
    static constexpr int MaxPly = 128;

    struct Entry {
        std::uint64_t key = 0;
        std::int16_t score = 0;
        std::int8_t depth = 0;
        std::uint8_t bound = 0;
        std::uint16_t move = 0; // the best move's squares and number of pieces taken
    };

    int Negamax(const Position& position, int depth, int alpha, int beta, int ply);
    bool IsRepetition(const Position& position) const;
    bool ShouldStop();
    int ScoreMove(const Position& position, const Move& move, int ply) const;
    void Store(std::uint64_t key, int depth, int score, int bound, int move, int ply);

    std::vector<Entry> m_table;
    std::vector<MoveList> m_lists; // one for each ply, to keep them off the stack
    std::vector<std::uint64_t> m_keys; // the game's and the search's, for repetitions
    std::array<std::array<std::uint16_t, 2>, MaxPly> m_killers{};
    std::array<std::array<int, SquareCount>, SquareCount> m_history{};
    Variant m_variant = Variant::Russian;
    const Rules* m_rules = &RulesOf(Variant::Russian);
    int m_rootDepth = 0;
    std::uint64_t m_nodes = 0;
    std::chrono::steady_clock::time_point m_deadline;
    std::stop_token m_stop;
    bool m_aborted = false;
};

// Runs an engine on a thread of its own; the window polls for the result.
class AiPlayer {
public:
    explicit AiPlayer(int hashMegabytes = 32) : m_engine(hashMegabytes) {}
    ~AiPlayer() { Cancel(); }
    AiPlayer(const AiPlayer&) = delete;
    AiPlayer& operator=(const AiPlayer&) = delete;

    void Start(const Position& position, Variant variant, std::vector<std::uint64_t> history, const SearchLimits& limits);
    void Cancel();
    void NewGame();
    bool Busy() const { return m_busy; }
    std::optional<SearchResult> TakeResult();

private:
    Engine m_engine;
    std::jthread m_worker;
    std::mutex m_mutex;
    std::optional<SearchResult> m_result;
    std::atomic<bool> m_busy = false;
};

} // namespace draughts
