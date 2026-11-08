#pragma once

#include "BoardGeometry.h"
#include "Draughts.h"
#include "Gl.h"
#include "MathTypes.h"
#include "PieceModels.h"
#include "Themes.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace render {

// One disc: a man, or one of the two discs of a king.
struct PieceDraw {
    draughts::Color color = draughts::Color::White;
    math::Mat4 model = math::Mat4::Identity();
    float ground = 0; // the height of what it stands on
    math::Vec3 glowColor;
    float glow = 0;
    float dissolve = 0;
    float crown = 0; // the crown inlaid on top, for the upper disc of a king
    float seed = 0;  // where the grain falls
};

// A soft dark patch round the foot of a piece.
struct ContactShadow {
    float x = 0, z = 0, radius = 0, strength = 0;
};

// Squares marked on the board, as bits of file + 8 * rank.
struct BoardMarks {
    std::uint64_t selected = 0;   // the piece picked up
    std::uint64_t quiet = 0;      // where it can move
    std::uint64_t capture = 0;    // where it can land taking something
    std::uint64_t route = 0;      // the landings chosen so far, for a capture with several routes
    std::uint64_t lastMove = 0;   // the squares of the last move
    std::uint64_t threatened = 0; // pieces that a move under the pointer would take
    std::uint64_t forced = 0;     // pieces that must take
    std::uint64_t hover = 0;
};

// An arrow along a route of square centers.
struct Arrow {
    std::vector<math::Vec3> route;
    math::Vec3 color;
    float alpha = 1;
};

// A rectangle of the screen behind which the scene is blurred, in pixels from the top left.
struct FrostedPanel {
    float x = 0, y = 0, width = 0, height = 0, radius = 0, opacity = 1;
};

struct Scene {
    math::Mat4 view = math::Mat4::Identity();
    math::Mat4 projection = math::Mat4::Identity();
    math::Vec3 eye;
    float time = 0;
    std::vector<PieceDraw> pieces;
    std::vector<ContactShadow> contacts;
    BoardMarks marks;
    std::optional<Arrow> arrow;
    std::vector<FrostedPanel> panels;
};

// A linked shader program, its sources taken from the executable's resources.
class Program {
public:
    struct Stage {
        gl::GLenum type;
        std::initializer_list<const wchar_t*> sources;
    };

    Program() = default;
    Program(std::string name, std::initializer_list<Stage> stages);
    // The GLSL version the shaders are compiled for, from the context's OpenGL version (46 for 4.6).
    static void SetVersion(int glVersion);

    gl::GLuint Id() const { return m_program.Id(); }
    void Use() const { gl::glUseProgram(m_program.Id()); }
    gl::GLint Location(const char* name) const;

    void Set(const char* name, int value) const;
    void Set(const char* name, float value) const;
    void Set(const char* name, math::Vec2 value) const;
    void Set(const char* name, math::Vec3 value) const;
    void Set(const char* name, math::Vec4 value) const;
    void Set(const char* name, const math::Mat4& value) const;
    void SetMask(const char* name, std::uint64_t bits) const;
    void SetArray(const char* name, std::span<const math::Vec4> values) const;

private:
    std::string m_name;
    gl::ProgramObject m_program;
    mutable std::unordered_map<std::string, gl::GLint> m_locations;
};

// Draws the scene into the window: shadows, reflections, the lit scene in high dynamic range with
// multisampling, then bloom and tone mapping. The interface is drawn on top afterwards.
class Renderer {
public:
    Renderer(const pieces::DiscShape& disc, int glVersion);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void SetTheme(const Theme& theme);
    void Resize(int width, int height);
    void Render(const Scene& scene);

    int Width() const { return m_width; }
    int Height() const { return m_height; }

private:
    struct GpuMesh {
        gl::VertexArray vertexArray;
        gl::Buffer vertices;
        gl::Buffer indices;
        gl::GLsizei count = 0;
        void Draw() const;
    };

    static GpuMesh Upload(const pieces::Mesh& mesh);
    static GpuMesh Upload(const board::Mesh& mesh);
    void CreateEnvironmentTextures();
    void BakeEnvironment();
    void CreateScreenTargets();
    void UpdateFrameData(const Scene& scene);
    void SetPass(math::Vec3 eye, bool shadows, bool mirror, bool display = false);
    void BindLighting(bool reflections) const;
    void DrawShadowMap(const Scene& scene);
    void DrawReflections(const Scene& scene);
    void DrawMainPass(const Scene& scene);
    void DrawPieces(const Scene& scene, bool detailed);
    void SetPieceMaterial(const PieceDraw& piece) const;
    void SetSurfaceMaterial(const Scene& scene) const;
    void PostProcess(const Scene& scene);

    int m_glVersion;
    int m_width = 0, m_height = 0;
    int m_samples = 4;
    Theme m_theme;
    math::Mat4 m_lightViewProjection;

    GpuMesh m_disc, m_simpleDisc;
    GpuMesh m_squares, m_frame, m_table, m_arrow;
    std::vector<math::Vec3> m_arrowRoute;
    gl::VertexArray m_emptyVertexArray;

    Program m_pieceProgram, m_surfaceProgram, m_glowProgram, m_shadowProgram, m_backgroundProgram;
    Program m_resolveProgram, m_bloomDownProgram, m_bloomUpProgram, m_compositeProgram;
    Program m_environmentProgram, m_irradianceProgram, m_prefilterProgram, m_brdfProgram;

    gl::Buffer m_frameData, m_passData;

    gl::Texture m_shadowMap;
    gl::Framebuffer m_shadowFramebuffer;
    gl::Sampler m_shadowCompareSampler, m_shadowDepthSampler;
    gl::Texture m_environment, m_irradiance, m_prefiltered, m_brdf, m_labels;
    int m_prefilteredLevels = 6;

    gl::Texture m_sceneColor, m_sceneDepth, m_resolved;
    gl::Framebuffer m_sceneFramebuffer, m_resolvedFramebuffer;
    gl::Texture m_reflectionColor, m_reflectionDepth;
    gl::Framebuffer m_reflectionFramebuffer;
    int m_reflectionLevels = 1;
    static constexpr int BloomLevels = 6;
    std::array<gl::Texture, BloomLevels> m_bloom;
    std::array<gl::Framebuffer, BloomLevels> m_bloomFramebuffers;
    std::array<std::array<int, 2>, BloomLevels> m_bloomSizes{};
};

} // namespace render
