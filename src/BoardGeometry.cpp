#include "BoardGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace board {

using namespace math;

namespace {

// Adds a quad, wound counterclockwise as seen from the side its normal faces.
void AddQuad(Mesh& mesh, const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d)
{
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {a, b, c, d});
    const Vec3 facing = Cross(b.position - a.position, c.position - a.position);
    const Vec3 normal = a.normal + b.normal + c.normal + d.normal;
    if (Dot(facing, normal) >= 0)
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    else
        mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
}

} // namespace

Mesh BuildSquares()
{
    Mesh mesh;
    const Vec3 up{0, 1, 0};
    AddQuad(mesh, {{-4, 0, 4}, up, {0, 0}}, {{4, 0, 4}, up, {1, 0}}, {{4, 0, -4}, up, {1, 1}}, {{-4, 0, -4}, up, {0, 1}});
    return mesh;
}

// The frame's cross-section, swept round the board with mitred corners: a rounded inner edge rising from
// the squares, a flat top with an eased outer edge, and the side down to the table. The inner edge is a
// quarter circle rather than a step, so that it does not shrink to a flickering sliver when seen from afar.
Mesh BuildFrame()
{
    struct ProfilePoint {
        float out, y;  // outward from the playing area's edge, and height
        float nOut, nY; // normal
    };
    constexpr std::array<ProfilePoint, 11> profile{{
        {0.000f, -0.002f, -1, 0},
        {0.000f, 0.000f, -1, 0},
        {0.0023f, 0.0115f, -0.924f, 0.383f},
        {0.0088f, 0.0212f, -0.707f, 0.707f},
        {0.0185f, 0.0277f, -0.383f, 0.924f},
        {0.030f, FrameTop, 0, 1},
        {0.585f, FrameTop, 0, 1},
        {0.607f, 0.022f, 0.62f, 0.78f},
        {0.618f, 0.006f, 0.96f, 0.28f},
        {0.62f, -0.012f, 1, 0},
        {0.62f, TableHeight, 1, 0},
    }};

    const std::array<Vec3, 4> outward{Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, 0, -1}, Vec3{-1, 0, 0}};
    Mesh mesh;
    for (const Vec3& o : outward) {
        const Vec3 along{-o.z, 0, o.x}; // along the side
        for (std::size_t i = 0; i + 1 < profile.size(); ++i) {
            const ProfilePoint& p = profile[i];
            const ProfilePoint& q = profile[i + 1];
            if (p.out == q.out && p.y == q.y)
                continue;
            const float pExtent = FrameInner + p.out, qExtent = FrameInner + q.out;
            auto vertex = [&](const ProfilePoint& point, float extent, float s) {
                return Vertex{o * extent + along * s + Vec3{0, point.y, 0}, Normalize(o * point.nOut + Vec3{0, point.nY, 0}), {s, point.out}};
            };
            AddQuad(mesh, vertex(p, pExtent, -pExtent), vertex(p, pExtent, pExtent), vertex(q, qExtent, qExtent), vertex(q, qExtent, -qExtent));
        }
    }
    return mesh;
}

Mesh BuildTable(float radius)
{
    Mesh mesh;
    const Vec3 up{0, 1, 0};
    constexpr int Segments = 96;
    mesh.vertices.push_back({{0, TableHeight, 0}, up, {0.5f, 0.5f}});
    for (int i = 0; i < Segments; ++i) {
        const float angle = 2 * Pi * static_cast<float>(i) / Segments;
        const float x = std::cos(angle) * radius, z = std::sin(angle) * radius;
        mesh.vertices.push_back({{x, TableHeight, z}, up, {x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f}});
    }
    for (std::uint32_t i = 0; i < Segments; ++i) {
        // Counterclockwise from above: from +x toward -z.
        const std::uint32_t a = 1 + i, b = 1 + (i + 1) % Segments;
        mesh.indices.insert(mesh.indices.end(), {0, b, a});
    }
    return mesh;
}

Mesh BuildArrow(std::span<const Vec3> route)
{
    Mesh mesh;
    if (route.size() < 2)
        return mesh;
    const Vec3 up{0, 1, 0};
    const Vec3 lift{0, 0.012f, 0};
    const float shaftHalf = 0.085f, headHalf = 0.24f, headLength = 0.42f;

    // The route, starting clear of the piece and stopping short of the last square's middle.
    std::vector<Vec3> points(route.begin(), route.end());
    for (Vec3& point : points)
        point.y = 0;
    const Vec3 firstDirection = Normalize(points[1] - points[0]);
    points.front() = points.front() + firstDirection * 0.28f;
    const Vec3 lastDirection = Normalize(points.back() - points[points.size() - 2]);
    const Vec3 tip = points.back() - lastDirection * 0.12f;
    points.back() = tip - lastDirection * headLength; // the neck, where the head begins

    float total = 0;
    for (std::size_t i = 1; i < points.size(); ++i)
        total += Length(points[i] - points[i - 1]);
    total += headLength;

    // The shaft: a ribbon, mitred at the turns.
    float travelled = 0;
    std::vector<Vertex> left, right;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec3 in = i > 0 ? Normalize(points[i] - points[i - 1]) : Normalize(points[1] - points[0]);
        const Vec3 out = i + 1 < points.size() ? Normalize(points[i + 1] - points[i]) : in;
        Vec3 direction = in + out;
        direction = Length(direction) > 1e-4f ? Normalize(direction) : out;
        const Vec3 side{-direction.z, 0, direction.x};
        const Vec3 sideIn{-in.z, 0, in.x};
        const float miter = std::min(1 / std::max(Dot(side, sideIn), 0.3f), 2.5f);
        if (i > 0)
            travelled += Length(points[i] - points[i - 1]);
        const float u = travelled / total;
        left.push_back({points[i] + lift - side * shaftHalf * miter, up, {u, -1}});
        right.push_back({points[i] + lift + side * shaftHalf * miter, up, {u, 1}});
    }
    auto addTriangle = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), {a, b, c});
        // Counterclockwise from above.
        if (Dot(Cross(b.position - a.position, c.position - a.position), up) > 0)
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
        else
            mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1});
    };
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        addTriangle(left[i], right[i], right[i + 1]);
        addTriangle(left[i], right[i + 1], left[i + 1]);
    }

    // The head.
    const Vec3 side{-lastDirection.z, 0, lastDirection.x};
    const float neckU = travelled / total;
    addTriangle({points.back() + lift - side * headHalf, up, {neckU, -1}}, {points.back() + lift + side * headHalf, up, {neckU, 1}},
                {tip + lift, up, {1, 0}});
    return mesh;
}

} // namespace board
