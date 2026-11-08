#include "BoardView.h"

#include "BoardGeometry.h"
#include "PieceModels.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace math;
using draughts::Color;

namespace {

float Ease(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
float Approach(float value, float target, float step) { return value < target ? std::min(value + step, target) : std::max(value - step, target); }

constexpr float CrownDrop = 0.9f; // the height the second disc of a king falls from
constexpr float CrownTime = 0.45f;
constexpr float FadeTime = 0.45f;
constexpr int PileHeight = 4; // discs

} // namespace

float BoardView::Random()
{
    m_seed = m_seed * 1664525u + 1013904223u;
    return static_cast<float>(m_seed >> 8) / static_cast<float>(1u << 24);
}

BoardView::Piece* BoardView::At(int square)
{
    for (Piece& piece : m_pieces)
        if (piece.square == square)
            return &piece;
    return nullptr;
}

float BoardView::GroundAt(Vec3 position)
{
    return std::abs(position.x) < board::FrameOuter && std::abs(position.z) < board::FrameOuter ? 0.0f : board::TableHeight;
}

float BoardView::HopDuration(float distance, bool jumping)
{
    return jumping ? 0.3f + 0.035f * distance : std::min(0.2f + 0.06f * distance, 0.6f);
}

float BoardView::RouteDuration(Vec3 start, std::span<const Vec3> points, bool jumping)
{
    float total = 0;
    for (const Vec3 point : points) {
        total += HopDuration(Length(point - start), jumping);
        start = point;
    }
    return total;
}

void BoardView::Send(Piece& piece, std::span<const Vec3> points, bool jumping, float delay, Event landing)
{
    piece.routeLength = static_cast<int>(std::min(points.size(), piece.route.size()));
    std::copy_n(points.begin(), piece.routeLength, piece.route.begin());
    piece.hop = 0;
    piece.hopTime = 0;
    piece.jumping = jumping;
    piece.delay = delay;
    piece.landing = landing;
    piece.held = false;
}

// The pieces taken stand in piles of four discs on the right of the player who took them; a king is two discs.
Vec3 BoardView::TablePosition(Color color, int order, bool king) const
{
    std::vector<std::pair<int, bool>> earlier;
    for (const Piece& piece : m_pieces)
        if (piece.square == -1 && piece.color == color && piece.tableOrder >= 0 && piece.tableOrder < order)
            earlier.emplace_back(piece.tableOrder, piece.king);
    std::ranges::sort(earlier);
    int pile = 0, height = 0;
    auto add = [&](bool isKing) {
        const int size = isKing ? 2 : 1;
        if (height + size > PileHeight) {
            ++pile;
            height = 0;
        }
        height += size;
    };
    for (const auto& [_, isKing] : earlier)
        add(isKing);
    add(king);
    const float y = board::TableHeight + static_cast<float>(height - (king ? 2 : 1)) * pieces::Height;
    const float step = 1.0f * static_cast<float>(pile);
    if (color == Color::Black)
        return {5.3f, y, 1.6f - step};
    return {-5.3f, y, -1.6f + step};
}

void BoardView::Place(Color color, bool king, int square, bool appear, float delay)
{
    Piece piece;
    piece.color = color;
    piece.king = king;
    piece.square = square;
    piece.position = board::SquareCenter(square);
    piece.yaw = Random() * 2 * Pi;
    piece.seed = Random();
    piece.crown = piece.crownTarget = king ? 1.0f : 0.0f;
    if (appear) {
        piece.dissolve = 1;
        piece.dissolveDelay = delay;
    }
    m_pieces.push_back(piece);
}

void BoardView::PutOnTable(Color color, bool king, bool appear, float delay)
{
    Piece piece;
    piece.color = color;
    piece.king = king;
    piece.square = -1;
    piece.tableOrder = m_tableCounts[std::to_underlying(color)]++;
    piece.position = TablePosition(color, piece.tableOrder, king);
    piece.yaw = Random() * 2 * Pi;
    piece.seed = Random();
    piece.crown = piece.crownTarget = king ? 1.0f : 0.0f;
    if (appear) {
        piece.dissolve = 1;
        piece.dissolveDelay = delay;
    }
    m_pieces.push_back(piece);
}

void BoardView::Rebuild(const draughts::Game& game, bool animate)
{
    if (animate) {
        for (Piece& piece : m_pieces) {
            piece.dissolveTarget = 1;
            piece.dissolveDelay = 0;
            piece.removeWhenGone = true;
            piece.square = -2;
            piece.tableOrder = -1;
        }
    }
    else {
        m_pieces.clear();
    }
    m_tableCounts = {};

    const draughts::Position& position = game.Current();
    for (draughts::Square square = 0; square < draughts::SquareCount; ++square)
        if (const auto piece = position.At(square))
            Place(piece->color, piece->king, square, animate, 0.3f + 0.02f * static_cast<float>(square));
    // The pieces taken so far, in the order they were taken.
    for (int ply = 0; ply < game.Ply(); ++ply) {
        const draughts::MoveRecord& record = game.Moves()[static_cast<std::size_t>(ply)];
        if (!record.move.IsCapture())
            continue;
        const Color taken = draughts::Opposite(game.PositionAt(ply).SideToMove());
        for (int i = 0; i < record.move.steps; ++i)
            PutOnTable(taken, (record.takenKings & draughts::Bit(record.move.taken[i])) != 0, animate, 0.5f);
    }
}

void BoardView::PlayMove(const draughts::Position& before, const draughts::MoveRecord& record, bool forward)
{
    const draughts::Move& move = record.move;
    const bool capture = move.IsCapture();
    const Color mover = before.SideToMove(), victims = draughts::Opposite(mover);
    const Vec3 from = board::SquareCenter(move.from);
    std::array<Vec3, 13> points{};
    for (int i = 0; i < move.steps; ++i)
        points[static_cast<std::size_t>(i)] = board::SquareCenter(move.path[static_cast<std::size_t>(i)]);
    const std::span<const Vec3> route(points.data(), static_cast<std::size_t>(move.steps));
    const float travel = RouteDuration(from, route, capture);

    if (forward) {
        Piece* piece = At(move.from);
        // Each piece taken glows as it is jumped, and leaves for the table when the move is over.
        float time = 0;
        Vec3 at = from;
        int count = 0;
        for (int i = 0; capture && i < move.steps; ++i) {
            const float hop = HopDuration(Length(points[static_cast<std::size_t>(i)] - at), true);
            if (Piece* victim = At(move.taken[static_cast<std::size_t>(i)])) {
                victim->flashDelay = time + hop * 0.5f;
                victim->square = -1;
                victim->tableOrder = m_tableCounts[std::to_underlying(victims)]++;
                const Vec3 place = TablePosition(victims, victim->tableOrder, victim->king);
                Send(*victim, {&place, 1}, true, travel + 0.15f + 0.12f * static_cast<float>(count++), Event::Pile);
            }
            time += hop;
            at = points[static_cast<std::size_t>(i)];
        }
        if (piece) {
            piece->square = move.To();
            Send(*piece, route, capture, 0, capture ? Event::Jump : Event::Step);
            if (move.crowns) {
                piece->king = true;
                piece->crownTarget = 1;
                piece->crownDelay = travel + 0.05f;
            }
        }
        return;
    }

    // Back: the crown comes off first, the piece retraces its route, and the pieces it took come back.
    float delay = 0;
    if (Piece* piece = At(move.To())) {
        if (move.crowns) {
            piece->king = false;
            piece->crownTarget = 0;
            piece->crownDelay = 0;
            delay = CrownTime * 0.8f;
        }
        std::array<Vec3, 13> back{};
        for (int i = 0; i < move.steps; ++i)
            back[static_cast<std::size_t>(i)] = i + 1 < move.steps ? points[static_cast<std::size_t>(move.steps - 2 - i)] : from;
        piece->square = move.from;
        Send(*piece, {back.data(), static_cast<std::size_t>(move.steps)}, capture, delay, capture ? Event::Jump : Event::Step);
    }
    const int count = move.CaptureCount();
    const int base = m_tableCounts[std::to_underlying(victims)] - count;
    for (int k = 0; capture && k < move.steps; ++k) {
        for (Piece& piece : m_pieces) {
            if (piece.square == -1 && piece.color == victims && piece.tableOrder == base + k) {
                piece.square = move.taken[static_cast<std::size_t>(k)];
                piece.tableOrder = -1;
                const Vec3 place = board::SquareCenter(piece.square);
                Send(piece, {&place, 1}, true, delay + 0.1f + 0.1f * static_cast<float>(count - 1 - k), Event::Step);
                break;
            }
        }
    }
    if (capture)
        m_tableCounts[std::to_underlying(victims)] = std::max(base, 0);
}

void BoardView::Update(float seconds, std::vector<Event>& events)
{
    for (Piece& piece : m_pieces) {
        if (piece.flashDelay >= 0) {
            piece.flashDelay -= seconds;
            if (piece.flashDelay < 0)
                piece.flash = 1.4f;
        }
        piece.flash = std::max(0.0f, piece.flash - seconds * 1.5f);

        if (piece.dissolveDelay > 0)
            piece.dissolveDelay -= seconds;
        else
            piece.dissolve = Approach(piece.dissolve, piece.dissolveTarget, seconds / FadeTime);

        if (piece.crownDelay > 0) {
            piece.crownDelay -= seconds;
        }
        else if (piece.crown != piece.crownTarget) {
            const float before = piece.crown;
            piece.crown = Approach(piece.crown, piece.crownTarget, seconds / CrownTime);
            if (before < 1 && piece.crown >= 1)
                events.push_back(Event::Crown);
        }

        if (piece.held)
            continue;
        if (piece.delay > 0) {
            piece.delay -= seconds;
            continue;
        }
        if (piece.hop < piece.routeLength) {
            if (piece.hopTime == 0)
                piece.hopStart = piece.position;
            const Vec3 target = piece.route[static_cast<std::size_t>(piece.hop)];
            const float distance = Length(target - piece.hopStart);
            const float duration = HopDuration(distance, piece.jumping);
            piece.hopTime = std::min(piece.hopTime + seconds, duration);
            const float t = piece.hopTime / duration;
            const float arc = piece.jumping ? 0.3f + 0.03f * distance : 0.025f + 0.012f * distance;
            piece.position = Lerp(piece.hopStart, target, Ease(t)) + Vec3{0, arc * std::sin(Pi * t), 0};
            if (t >= 1) {
                piece.position = target;
                ++piece.hop;
                piece.hopTime = 0;
                events.push_back(piece.landing);
            }
        }
    }
    std::erase_if(m_pieces, [](const Piece& piece) { return piece.removeWhenGone && piece.dissolve >= 1; });
}

bool BoardView::Animating() const
{
    return std::ranges::any_of(m_pieces, [](const Piece& piece) {
        return piece.delay > 0 || piece.hop < piece.routeLength || piece.crown != piece.crownTarget || piece.dissolve != piece.dissolveTarget ||
               piece.flashDelay >= 0 || piece.flash > 0;
    });
}

void BoardView::Hold(draughts::Square square, Vec3 position)
{
    if (Piece* piece = At(square)) {
        piece->held = true;
        piece->routeLength = piece->hop = 0;
        piece->position = position;
    }
}

void BoardView::Release(draughts::Square square)
{
    if (Piece* piece = At(square)) {
        const Vec3 place = board::SquareCenter(square);
        Send(*piece, {&place, 1}, false, 0, Event::Step);
    }
}

int BoardView::Pick(Vec3 origin, Vec3 direction) const
{
    int best = -1;
    float nearest = 1e9f;
    for (const Piece& piece : m_pieces) {
        if (piece.square < 0 || piece.dissolve > 0.5f)
            continue;
        const float radius = pieces::Radius * 0.95f;
        const float height = pieces::Height * (piece.crown > 0.5f ? 2.0f : 1.0f);
        const Vec3 o = origin - piece.position;
        // The upright cylinder of the piece: its side, then its top.
        const float a = direction.x * direction.x + direction.z * direction.z;
        const float b = 2 * (o.x * direction.x + o.z * direction.z);
        const float c = o.x * o.x + o.z * o.z - radius * radius;
        float hit = 1e9f;
        if (a > 1e-8f) {
            const float discriminant = b * b - 4 * a * c;
            if (discriminant >= 0) {
                const float t = (-b - std::sqrt(discriminant)) / (2 * a);
                const float y = o.y + direction.y * t;
                if (t > 0 && y >= 0 && y <= height)
                    hit = t;
            }
        }
        if (std::abs(direction.y) > 1e-6f) {
            const float t = (height - o.y) / direction.y;
            const float x = o.x + direction.x * t, z = o.z + direction.z * t;
            if (t > 0 && x * x + z * z <= radius * radius)
                hit = std::min(hit, t);
        }
        if (hit < nearest) {
            nearest = hit;
            best = piece.square;
        }
    }
    return best;
}

std::vector<render::PieceDraw> BoardView::Draws(int selected, int hovered, std::uint32_t threatened, Vec3 accent, float time) const
{
    const Vec3 danger{1.0f, 0.28f, 0.1f};
    std::vector<render::PieceDraw> draws;
    draws.reserve(m_pieces.size() * 2);
    for (const Piece& piece : m_pieces) {
        render::PieceDraw draw;
        draw.color = piece.color;
        draw.model = Mat4::Translation(piece.position) * Mat4::RotationY(piece.yaw);
        draw.ground = GroundAt(piece.position);
        draw.dissolve = piece.dissolve;
        draw.seed = piece.seed;
        draw.glowColor = accent;
        if (piece.square >= 0 && piece.square == selected)
            draw.glow = 0.8f + 0.2f * std::sin(time * 4.0f);
        else if (piece.square >= 0 && piece.square == hovered)
            draw.glow = 0.35f;
        if (piece.square >= 0 && (threatened & draughts::Bit(piece.square)) != 0) {
            draw.glowColor = danger;
            draw.glow = 0.5f + 0.15f * std::sin(time * 5.0f);
        }
        if (piece.flash > 0) {
            draw.glowColor = danger;
            draw.glow = std::max(draw.glow, std::min(piece.flash, 1.0f) * 1.3f);
        }
        draws.push_back(draw);

        if (piece.crown > 0) {
            // The second disc of a king, with the crown on top, turned to face its owner.
            render::PieceDraw top = draw;
            const float drop = (1 - EaseOut(piece.crown)) * CrownDrop;
            top.model = Mat4::Translation(piece.position + Vec3{0, pieces::Height + drop, 0}) * Mat4::RotationY(piece.color == Color::White ? 0.0f : Pi);
            top.dissolve = std::max(piece.dissolve, 1 - piece.crown);
            top.crown = 1;
            top.seed = piece.seed + 0.37f;
            draws.push_back(top);
        }
    }
    return draws;
}

std::vector<render::ContactShadow> BoardView::Contacts() const
{
    std::vector<render::ContactShadow> contacts;
    for (const Piece& piece : m_pieces) {
        const float height = piece.position.y - GroundAt(piece.position);
        const float strength = 0.72f * (1 - SmoothStep(0.0f, 0.8f, height)) * (1 - piece.dissolve);
        if (strength > 0.01f)
            contacts.push_back({piece.position.x, piece.position.z, pieces::Radius * (1.05f + height * 0.6f), strength});
    }
    return contacts;
}
