#include "PieceModels.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pieces {

using namespace math;

namespace {

struct ProfilePoint {
    float r = 0, y = 0;
    float occlusion = 1;
};

// A smooth dip of width 2 * halfWidth: 1 in the middle, 0 at the edges.
float Bump(float x, float center, float halfWidth)
{
    const float t = (x - center) / halfWidth;
    return std::abs(t) < 1 ? (1 - t * t) * (1 - t * t) : 0.0f;
}

constexpr float EdgeRadius = 0.026f;  // the rounded lower edge
constexpr float RimRadius = 0.032f;   // the rounded upper edge
constexpr float Recess = 0.245f;      // the radius of the dished middle of the top

// The height of the top at a distance from the axis: a groove near the rim, a step down into a shallow
// dish, and a fine ring in it.
float TopHeight(float r)
{
    float y = Height;
    y -= 0.007f * Bump(r, 0.305f, 0.010f);
    y -= 0.010f * SmoothStep(Recess + 0.016f, Recess, r);
    if (r < Recess)
        y -= 0.006f * (1 - (r / Recess) * (r / Recess));
    y -= 0.0028f * Bump(r, 0.155f, 0.007f);
    return y;
}

float TopOcclusion(float r) { return 1 - 0.45f * Bump(r, 0.305f, 0.012f) - 0.3f * Bump(r, Recess + 0.004f, 0.012f) - 0.25f * Bump(r, 0.155f, 0.008f); }

// The side, between the two rounded edges, with a groove near the top and one near the bottom.
float SideRadius(float y) { return Radius - 0.005f * Bump(y, 0.046f, 0.008f) - 0.005f * Bump(y, Height - 0.052f, 0.008f); }
float SideOcclusion(float y) { return 1 - 0.35f * Bump(y, 0.046f, 0.009f) - 0.35f * Bump(y, Height - 0.052f, 0.009f); }

// The outline from the middle of the bottom, round the edge and over the top to the middle of the top.
std::vector<ProfilePoint> Profile(int detail)
{
    std::vector<ProfilePoint> points;
    const auto count = [&](int n) { return std::max(2, n * detail / 4); };

    // The bottom, covered with cloth.
    const int bottom = count(4);
    for (int i = 0; i < bottom; ++i)
        points.push_back({(Radius - EdgeRadius) * static_cast<float>(i) / static_cast<float>(bottom), 0.0f});
    // The lower edge.
    const int edge = count(8);
    for (int i = 0; i < edge; ++i) {
        const float angle = -Pi / 2 + Pi / 2 * static_cast<float>(i) / static_cast<float>(edge);
        points.push_back({Radius - EdgeRadius + EdgeRadius * std::cos(angle), EdgeRadius + EdgeRadius * std::sin(angle)});
    }
    // The side.
    const int side = count(28);
    for (int i = 0; i < side; ++i) {
        const float y = Lerp(EdgeRadius, Height - RimRadius, static_cast<float>(i) / static_cast<float>(side));
        points.push_back({SideRadius(y), y, SideOcclusion(y)});
    }
    // The upper edge.
    const int rim = count(10);
    for (int i = 0; i < rim; ++i) {
        const float angle = Pi / 2 * static_cast<float>(i) / static_cast<float>(rim);
        points.push_back({Radius - RimRadius + RimRadius * std::cos(angle), Height - RimRadius + RimRadius * std::sin(angle)});
    }
    // The top, inwards.
    const int top = count(90);
    for (int i = 0; i <= top; ++i) {
        const float r = (Radius - RimRadius) * (1 - static_cast<float>(i) / static_cast<float>(top));
        points.push_back({r, TopHeight(r), TopOcclusion(r)});
    }
    return points;
}

// The profile turned round the vertical axis. Normals come from the neighbouring points of the outline.
Mesh Lathe(const std::vector<ProfilePoint>& profile, int segments)
{
    Mesh mesh;
    const int count = static_cast<int>(profile.size());
    std::vector<Vec2> normals(profile.size());
    for (int i = 0; i < count; ++i) {
        const ProfilePoint& before = profile[static_cast<std::size_t>(std::max(i - 1, 0))];
        const ProfilePoint& after = profile[static_cast<std::size_t>(std::min(i + 1, count - 1))];
        const Vec2 tangent{after.r - before.r, after.y - before.y};
        const float length = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
        normals[static_cast<std::size_t>(i)] = length > 0 ? Vec2{tangent.y / length, -tangent.x / length} : Vec2{0, 1};
    }
    // The middles of the bottom and of the top face straight down and up.
    normals.front() = {0, -1};
    normals.back() = {0, 1};

    for (int i = 0; i < count; ++i) {
        const ProfilePoint& point = profile[static_cast<std::size_t>(i)];
        const Vec2 n = normals[static_cast<std::size_t>(i)];
        for (int j = 0; j <= segments; ++j) {
            const float angle = 2 * Pi * static_cast<float>(j) / static_cast<float>(segments);
            const float c = std::cos(angle), s = std::sin(angle);
            mesh.vertices.push_back({{point.r * c, point.y, point.r * s}, Normalize(Vec3{n.x * c, n.y, n.x * s}), point.occlusion});
        }
    }
    const auto index = [&](int i, int j) { return static_cast<std::uint32_t>(i * (segments + 1) + j); };
    for (int i = 0; i + 1 < count; ++i) {
        for (int j = 0; j < segments; ++j) {
            // Next to the axis a quad narrows to a triangle.
            if (profile[static_cast<std::size_t>(i)].r > 0)
                mesh.indices.insert(mesh.indices.end(), {index(i, j), index(i + 1, j + 1), index(i, j + 1)});
            if (profile[static_cast<std::size_t>(i + 1)].r > 0)
                mesh.indices.insert(mesh.indices.end(), {index(i, j), index(i + 1, j), index(i + 1, j + 1)});
        }
    }
    return mesh;
}

} // namespace

DiscShape BuildDisc()
{
    return {Lathe(Profile(4), 96), Lathe(Profile(1), 32)};
}

} // namespace pieces
