#pragma once

#include "MathTypes.h"

#include <cstdint>
#include <vector>

// The pieces: a draughtsman turned on a lathe, with a groove round its side and rings on its top.
// A king is two of them, one on the other.
namespace pieces {

struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    float occlusion = 1; // light reaching into the grooves
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

// A square of the board is 1 wide.
constexpr float Radius = 0.4f;
constexpr float Height = 0.15f;

struct DiscShape {
    Mesh detailed; // for the scene
    Mesh simple;   // for the shadows and the reflections
};

DiscShape BuildDisc();

} // namespace pieces
