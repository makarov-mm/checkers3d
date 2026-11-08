#pragma once

#include "Draughts.h"
#include "MathTypes.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

// The board, its frame, the table and the hint arrow. A square is one unit; the playing surface is the
// plane y = 0, a1 is at the left front corner seen from White, and White sits on the +z side.
namespace board {

constexpr float FrameInner = 4.0f;
constexpr float FrameOuter = 4.62f;
constexpr float FrameTop = 0.03f;
constexpr float TableHeight = -0.32f;

struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

inline math::Vec3 SquareCenter(draughts::Square square)
{
    return {static_cast<float>(draughts::FileOf(square)) - 3.5f, 0, 3.5f - static_cast<float>(draughts::RankOf(square))};
}

// The squares counted over the whole board, light ones too: file + 8 * rank. The marks on the board use these.
constexpr int BoardIndex(draughts::Square square) { return draughts::FileOf(square) + 8 * draughts::RankOf(square); }

// The dark square under a point of the playing surface, or -1 for a light square or none.
inline draughts::Square SquareAt(math::Vec3 point)
{
    const int file = static_cast<int>(std::floor(point.x + 4));
    const int rank = static_cast<int>(std::floor(4 - point.z));
    return draughts::SquareAt(file, rank);
}

Mesh BuildSquares();
Mesh BuildFrame();
Mesh BuildTable(float radius);
// An arrow lying on the board along a route of square centers, with its head at the last.
Mesh BuildArrow(std::span<const math::Vec3> route);

} // namespace board
