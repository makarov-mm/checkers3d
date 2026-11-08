#pragma once

#include "Draughts.h"
#include "MathTypes.h"
#include "Renderer.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// The pieces as they stand on the board and as they move: gliding, hopping over the pieces they take,
// stacking up when crowned, the pieces taken flying to piles beside the board, and everything dissolving
// in when a game is set out.
class BoardView {
public:
    // What happened in a frame, for the sounds.
    enum class Event { Step, Jump, Crown, Pile };

    // Sets out the game's position and the pieces taken so far.
    void Rebuild(const draughts::Game& game, bool animate);
    // A move played (forward) or taken back; `before` is the position before the move either way.
    void PlayMove(const draughts::Position& before, const draughts::MoveRecord& record, bool forward);
    void Update(float seconds, std::vector<Event>& events);
    bool Animating() const;

    // A piece held by the mouse, and let go again.
    void Hold(draughts::Square square, math::Vec3 position);
    void Release(draughts::Square square);

    // The square of the piece a ray hits first, or -1.
    int Pick(math::Vec3 origin, math::Vec3 direction) const;

    // `threatened`: the squares of pieces that a move under consideration would take.
    std::vector<render::PieceDraw> Draws(int selected, int hovered, std::uint32_t threatened, math::Vec3 accent, float time) const;
    std::vector<render::ContactShadow> Contacts() const;

private:
    struct Piece {
        draughts::Color color = draughts::Color::White;
        bool king = false;
        int square = -1;     // on the board; -1 on the table; -2 leaving
        int tableOrder = -1; // its place among the pieces of its color taken
        float yaw = 0;       // each piece turned its own way, so that the grain differs
        float seed = 0;
        math::Vec3 position;
        // Where it is going: hop after hop through the points of the route.
        std::array<math::Vec3, 14> route{};
        int routeLength = 0;
        int hop = 0;
        float hopTime = 0;
        math::Vec3 hopStart;
        bool jumping = false; // over pieces, rather than gliding
        Event landing = Event::Step;
        float delay = 0;
        // The second disc of a king, settled on top (1) or gone (0).
        float crown = 0, crownTarget = 0, crownDelay = 0;
        float dissolve = 0, dissolveTarget = 0, dissolveDelay = 0;
        float flash = 0, flashDelay = -1; // a glow when it is jumped
        bool held = false;
        bool removeWhenGone = false;
    };

    Piece* At(int square);
    void Place(draughts::Color color, bool king, int square, bool appear, float delay);
    void PutOnTable(draughts::Color color, bool king, bool appear, float delay);
    math::Vec3 TablePosition(draughts::Color color, int order, bool king) const;
    static float HopDuration(float distance, bool jumping);
    static float RouteDuration(math::Vec3 start, std::span<const math::Vec3> points, bool jumping);
    static void Send(Piece& piece, std::span<const math::Vec3> points, bool jumping, float delay, Event landing);
    float Random();
    static float GroundAt(math::Vec3 position);

    std::vector<Piece> m_pieces;
    std::array<int, 2> m_tableCounts{};
    std::uint32_t m_seed = 1;
};
