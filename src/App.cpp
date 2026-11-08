#include "App.h"

#include "BoardGeometry.h"
#include "PieceModels.h"
#include "Themes.h"

#include <dwmapi.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <random>
#include <stdexcept>
#include <utility>

using namespace math;
using draughts::Color;
using draughts::Variant;
using ui::FontId;
using ui::Rect;
using ui::Rgba;

namespace {

constexpr wchar_t ClassName[] = L"Checkers3D.Window";
constexpr float PanelWidth = 344, PanelMargin = 16;

enum Widget {
    HelpButton = 1,
    UndoButton,
    RedoButton,
    HintButton,
    NewGameButton,
    FlipButton,
    ModeSelector,
    LevelSelector = ModeSelector + 2,
    RulesSelector = LevelSelector + 2,
    ThemeSelector = RulesSelector + 2,
    SoundButton = ThemeSelector + 2,
    LanguageButton,
    ModalWidgets = 100, // the widgets of the help
    HelpClose,
};

const ui::Color TextColor = Rgba(236, 239, 245);
const ui::Color MutedColor = Rgba(150, 158, 176);

draughts::SearchLimits LevelLimits(int level, std::uint64_t seed)
{
    switch (level) {
    case 1: return {2, 3000, 150, seed};
    case 2: return {4, 3000, 60, seed};
    case 3: return {8, 2500, 20, seed};
    case 4: return {64, 1200, 0, seed};
    default: return {64, 3500, 0, seed};
    }
}

std::uint64_t BoardBit(int square) { return square >= 0 ? std::uint64_t{1} << board::BoardIndex(square) : 0; }

std::uint64_t BoardBits(std::uint32_t squares)
{
    std::uint64_t bits = 0;
    for (; squares; squares &= squares - 1)
        bits |= BoardBit(std::countr_zero(squares));
    return bits;
}

std::vector<Vec3> RouteOf(const draughts::Move& move)
{
    std::vector<Vec3> route{board::SquareCenter(move.from)};
    for (int i = 0; i < move.steps; ++i)
        route.push_back(board::SquareCenter(move.path[static_cast<std::size_t>(i)]));
    return route;
}

} // namespace

// Setup --------------------------------------------------------------------------------------------

App::App(HINSTANCE instance) : m_instance(instance)
{
    m_settings = Settings::Load();
    m_language = m_settings.language;
    m_theme = m_settings.theme;
    m_level = m_settings.level;
    m_mode = m_settings.mode;
    m_sound.enabled = m_settings.sound;
    m_panelVisible = m_settings.panel;
    m_panelSlide = m_panelVisible ? 1.0f : 0.0f;

    CreateMainWindow();
    m_context = std::make_unique<gl::Context>(m_window);
    render::Program::SetVersion(m_context->Version());
    m_painter = std::make_unique<ui::Painter>();
    BuildFonts();
    ShowWindow(m_window, m_settings.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(m_window);
    ShowLoading();

    m_renderer = std::make_unique<render::Renderer>(pieces::BuildDisc(), m_context->Version());
    m_renderer->SetTheme(Themes()[m_theme]);
    m_view = std::make_unique<BoardView>();

    if (auto saved = draughts::Game::Deserialize(m_settings.game))
        m_game = std::move(*saved);
    else
        m_game = draughts::Game(static_cast<Variant>(m_settings.variant));
    m_announcedEnd = m_game.Status().Over();
    m_view->Rebuild(m_game, true);
    m_camera.Reset(m_mode == PlayBlack ? Color::Black : Color::White);
    m_lastFrame = std::chrono::steady_clock::now();
}

App::~App()
{
    m_computer.Cancel();
    m_hinter.Cancel();
}

void App::CreateMainWindow()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = m_instance;
    windowClass.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = ClassName;
    RegisterClassExW(&windowClass);

    RECT rect{};
    if (m_settings.hasWindow && MonitorFromRect(&m_settings.window, MONITOR_DEFAULTTONULL)) {
        rect = m_settings.window;
    }
    else {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int dpi = static_cast<int>(GetDpiForSystem());
        const int workWidth = work.right - work.left, workHeight = work.bottom - work.top;
        const int width = std::min(MulDiv(1500, dpi, 96), workWidth * 92 / 100);
        const int height = std::min(MulDiv(920, dpi, 96), workHeight * 92 / 100);
        rect = {work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2, 0, 0};
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
    }

    m_window = CreateWindowExW(0, ClassName, L"3D Checkers", WS_OVERLAPPEDWINDOW, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                               nullptr, m_instance, this);
    if (!m_window)
        throw std::runtime_error("The window could not be created.");
    m_dpi = GetDpiForWindow(m_window);
    m_placement.length = sizeof(m_placement);
    // A dark title bar, to go with the game, where Windows offers one.
    constexpr DWORD UseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE, Windows 10 20H1+
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_window, UseImmersiveDarkMode, &dark, sizeof(dark));
}

void App::BuildFonts()
{
    const float scale = Scale();
    m_fonts[0] = std::make_unique<text::Font>(L"Segoe UI", 15 * scale, FW_NORMAL);
    m_fonts[1] = std::make_unique<text::Font>(L"Segoe UI", 19 * scale, FW_SEMIBOLD);
    m_fonts[2] = std::make_unique<text::Font>(L"Segoe UI", 31 * scale, FW_SEMIBOLD);
    m_painter->SetFonts(m_fonts[0].get(), m_fonts[1].get(), m_fonts[2].get());
    m_fontDpi = m_dpi;
}

void App::ShowLoading()
{
    RECT client{};
    GetClientRect(m_window, &client);
    m_width = client.right;
    m_height = client.bottom;
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, m_width, m_height);
    gl::glClearColor(0.016f, 0.017f, 0.022f, 1);
    gl::glClear(gl::GL_COLOR_BUFFER_BIT);
    m_painter->Begin(m_width, m_height);
    const Rect screen{0, 0, static_cast<float>(m_width), static_cast<float>(m_height)};
    m_painter->TextCentered(FontId::Display, screen, T(Str::Title), TextColor);
    m_painter->Flush();
    m_context->SwapBuffers();
}

int App::Run()
{
    MSG message{};
    while (m_running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                m_running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!m_running)
            break;
        if (IsIconic(m_window)) {
            WaitMessage();
            m_lastFrame = std::chrono::steady_clock::now();
            continue;
        }

        // Nothing moving and nothing new: wait for input instead of drawing the same picture again.
        if (!m_dirty && !NeedsAnimation()) {
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            m_lastFrame = std::chrono::steady_clock::now(); // the idle time is not animation time
            continue;
        }
        m_dirty = false;
        const auto start = std::chrono::steady_clock::now();
        Frame();
        // Swapping the buffers waits for the screen; where it does not, the loop sleeps a little.
        if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2))
            Sleep(1);
    }
    return 0;
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        static_cast<App*>(create->lpCreateParams)->m_window = window;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    m_dirty = true;
    auto pointerAt = [&] {
        m_pointer.x = static_cast<float>(static_cast<short>(LOWORD(lParam)));
        m_pointer.y = static_cast<float>(static_cast<short>(HIWORD(lParam)));
    };

    switch (message) {
    case WM_CLOSE:
        Save();
        DestroyWindow(m_window);
        return 0;
    case WM_DESTROY:
        m_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        m_width = LOWORD(lParam);
        m_height = HIWORD(lParam);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(m_window, &paint);
        EndPaint(m_window, &paint);
        // While the window is being resized the loop does not run; the frames come from here.
        if (m_renderer && m_width > 0 && m_height > 0)
            Frame();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        m_dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {MulDiv(900, static_cast<int>(m_dpi), 96), MulDiv(600, static_cast<int>(m_dpi), 96)};
        return 0;
    }
    case WM_MOUSEMOVE:
        pointerAt();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        pointerAt();
        SetCapture(m_window);
        m_pointer.leftDown = m_pointer.leftPressed = true;
        return 0;
    case WM_LBUTTONUP:
        pointerAt();
        m_pointer.leftDown = false;
        m_pointer.leftReleased = true;
        if (!m_pointer.rightDown)
            ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        pointerAt();
        SetCapture(m_window);
        m_pointer.rightDown = m_pointer.rightPressed = true;
        return 0;
    case WM_RBUTTONUP:
        pointerAt();
        m_pointer.rightDown = false;
        m_pointer.rightReleased = true;
        if (!m_pointer.leftDown)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (m_pointer.leftDown)
            m_pointer.leftReleased = true;
        if (m_pointer.rightDown)
            m_pointer.rightReleased = true;
        m_pointer.leftDown = m_pointer.rightDown = false;
        return 0;
    case WM_MOUSEWHEEL:
        m_pointer.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        return 0;
    case WM_KEYDOWN:
        HandleKey(wParam);
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN)) {
            ToggleFullScreen();
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, m_cursorHand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

// The frame ----------------------------------------------------------------------------------------

void App::Frame()
{
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::min(std::chrono::duration<float>(now - m_lastFrame).count(), 0.1f);
    m_lastFrame = now;
    m_time += seconds;
    if (m_width <= 0 || m_height <= 0)
        return;
    if (m_dpi != m_fontDpi)
        BuildFonts();
    m_renderer->Resize(m_width, m_height);

    Update(seconds);

    const Theme& theme = Themes()[m_theme];
    const std::uint32_t threatened = ThreatenedBy(m_hoverSquare);
    render::Scene scene;
    scene.view = m_camera.View();
    scene.projection = m_camera.Projection(static_cast<float>(m_width) / static_cast<float>(m_height));
    scene.eye = m_camera.Eye();
    scene.time = m_time;
    scene.pieces = m_view->Draws(m_selected, m_hoverPiece, threatened, theme.accent, m_time);
    scene.contacts = m_view->Contacts();

    // The marks: the selected piece and where it can go, the last move, and the pieces that must take.
    scene.marks.selected = BoardBit(m_selected);
    for (const draughts::Move& move : Candidates()) {
        (move.IsCapture() ? scene.marks.capture : scene.marks.quiet) |= BoardBit(move.To());
        if (move.steps > static_cast<int>(m_route.size()) + 1)
            scene.marks.quiet |= BoardBit(move.path[m_route.size()]); // the next landing on a longer route
    }
    for (const int square : m_route)
        scene.marks.route |= BoardBit(square);
    if (!m_game.Moves().empty()) {
        const draughts::Move& last = m_game.Moves().back().move;
        scene.marks.lastMove = BoardBit(last.from);
        for (int i = 0; i < last.steps; ++i)
            scene.marks.lastMove |= BoardBit(last.path[static_cast<std::size_t>(i)]);
    }
    if (HumanToMove() && !m_view->Animating())
        scene.marks.forced = BoardBits(m_game.Current().PiecesThatCanTake(m_game.GetRules()));
    scene.marks.threatened = BoardBits(threatened);
    scene.marks.hover = BoardBit(m_hoverSquare);
    if (m_hint && m_hintGlow > 0.01f)
        scene.arrow = render::Arrow{RouteOf(*m_hint), theme.accent * 3.5f, 0.9f * m_hintGlow};
    scene.panels = FrostedPanels();
    m_renderer->Render(scene);

    m_cursorHand = false;
    m_painter->Begin(m_width, m_height);
    DrawInterface();
    m_painter->Flush();
    HandleBoardPointer();

    m_pointer.leftPressed = m_pointer.leftReleased = m_pointer.rightPressed = m_pointer.rightReleased = false;
    m_pointer.wheel = 0;
    m_pointer.consumed = false;
    m_context->SwapBuffers();
}

void App::Update(float seconds)
{
    const float target = m_panelVisible ? 1.0f : 0.0f;
    m_panelSlide += (target - m_panelSlide) * (1 - std::exp(-seconds * 12));
    if (std::abs(target - m_panelSlide) < 0.001f)
        m_panelSlide = target;
    const float panelSpace = (PanelWidth + PanelMargin) * Scale() * m_panelSlide;
    m_camera.SetSpace(1 - panelSpace / static_cast<float>(std::max(m_width, 1)));
    m_camera.Update(seconds);

    m_events.clear();
    m_view->Update(seconds, m_events);
    for (const BoardView::Event event : m_events) {
        switch (event) {
        case BoardView::Event::Step: m_sound.Play(SoundBank::Effect::Step); break;
        case BoardView::Event::Jump: m_sound.Play(SoundBank::Effect::Jump); break;
        case BoardView::Event::Crown: m_sound.Play(SoundBank::Effect::Crown); break;
        case BoardView::Event::Pile: m_sound.Play(SoundBank::Effect::Pile); break;
        }
    }

    const bool over = m_game.Status().Over() && !m_view->Animating();
    m_bannerFade = std::clamp(m_bannerFade + (over ? seconds : -seconds) * 3, 0.0f, 1.0f);
    m_hintGlow = std::clamp(m_hintGlow + (m_hint ? seconds : -seconds) * 4, 0.0f, 1.0f);
    for (Toast& toast : m_toasts)
        toast.age += seconds;
    std::erase_if(m_toasts, [](const Toast& toast) { return toast.age > 3.2f; });

    if (m_hintThinking) {
        if (auto result = m_hinter.TakeResult()) {
            m_hintThinking = false;
            if (!result->best.IsNull() && HumanToMove()) {
                m_hint = result->best;
                ShowToast(std::string(T(Str::HintToast)) + draughts::ToNotation(result->best, m_game.GetVariant()));
            }
        }
        else if (!m_hinter.Busy()) {
            m_hintThinking = false;
        }
    }
    UpdateComputer(seconds);

    if (over && !m_announcedEnd) {
        m_announcedEnd = true;
        m_sound.Play(SoundBank::Effect::GameOver);
    }
    if (!m_game.Status().Over())
        m_announcedEnd = false;
}

bool App::NeedsAnimation() const
{
    return m_camera.Moving() || m_view->Animating() || m_computerThinking || m_computerResult || m_hintThinking || m_selected >= 0 ||
           !m_toasts.empty() || (m_panelSlide > 0 && m_panelSlide < 1) || (m_bannerFade > 0 && m_bannerFade < 1) || m_hintGlow > 0 ||
           m_gesture != Gesture::None || m_pointer.leftDown || m_pointer.rightDown || m_hoverSquare >= 0 ||
           (HumanToMove() && m_game.Current().HasCapture(m_game.GetRules())) || (!m_game.Status().Over() && !IsHuman(m_game.Current().SideToMove()));
}

// The game -----------------------------------------------------------------------------------------

bool App::IsHuman(Color side) const
{
    switch (m_mode) {
    case PlayWhite: return side == Color::White;
    case PlayBlack: return side == Color::Black;
    case TwoPlayers: return true;
    default: return false;
    }
}

bool App::HumanToMove() const
{
    return !m_game.Status().Over() && IsHuman(m_game.Current().SideToMove());
}

void App::StopComputer()
{
    m_computer.Cancel();
    m_computerThinking = false;
    m_computerResult.reset();
    m_hinter.Cancel();
    m_hintThinking = false;
}

void App::UpdateComputer(float seconds)
{
    if (m_game.Status().Over() || IsHuman(m_game.Current().SideToMove()))
        return;
    m_computerClock += seconds;
    if (!m_computerThinking && !m_computerResult) {
        static std::mt19937_64 random{std::random_device{}()};
        m_computer.Start(m_game.Current(), m_game.GetVariant(), m_game.RepetitionHistory(), LevelLimits(m_level, random()));
        m_computerThinking = true;
        m_computerClock = 0;
    }
    if (m_computerThinking) {
        if (auto result = m_computer.TakeResult()) {
            m_computerResult = result;
            m_computerThinking = false;
        }
        else if (!m_computer.Busy()) {
            m_computerThinking = false; // cancelled; a new search starts next frame
        }
    }
    const float pause = m_mode == Demo ? 0.9f : 0.45f;
    if (m_computerResult && !m_view->Animating() && m_computerClock >= pause && m_gesture == Gesture::None) {
        const draughts::Move move = m_computerResult->best;
        m_computerResult.reset();
        if (!move.IsNull())
            PlayMove(move);
    }
}

void App::NewGame()
{
    StopComputer();
    m_computer.NewGame();
    const Variant variant = m_game.GetVariant();
    m_game.Reset(variant, draughts::Position::Start(variant));
    m_view->Rebuild(m_game, true);
    m_camera.Reset(m_mode == PlayBlack ? Color::Black : Color::White);
    m_announcedEnd = false;
    AfterPositionChange();
}

void App::PlayMove(const draughts::Move& move)
{
    const draughts::Position before = m_game.Current();
    if (!m_game.Play(move))
        return;
    m_view->PlayMove(before, m_game.Moves().back(), true);
    AfterPositionChange();
}

void App::Undo()
{
    StopComputer();
    if (!m_game.CanUndo()) {
        ShowToast(std::string(T(Str::NothingToUndo)));
        return;
    }
    auto undoOne = [&] {
        const draughts::MoveRecord record = m_game.Moves().back();
        m_game.Undo();
        m_view->PlayMove(m_game.Current(), record, false);
    };
    undoOne();
    // Against the computer, back to the player's own turn.
    if ((m_mode == PlayWhite || m_mode == PlayBlack) && !IsHuman(m_game.Current().SideToMove()) && m_game.CanUndo())
        undoOne();
    AfterPositionChange();
}

void App::Redo()
{
    StopComputer();
    if (!m_game.CanRedo())
        return;
    auto redoOne = [&] {
        const draughts::Position before = m_game.Current();
        m_game.Redo();
        m_view->PlayMove(before, m_game.Moves().back(), true);
    };
    redoOne();
    if ((m_mode == PlayWhite || m_mode == PlayBlack) && !IsHuman(m_game.Current().SideToMove()) && m_game.CanRedo())
        redoOne();
    AfterPositionChange();
}

void App::SetMode(int mode)
{
    StopComputer();
    m_mode = (mode % 4 + 4) % 4;
    if (m_mode == PlayWhite || m_mode == PlayBlack)
        m_camera.FaceSide(m_mode == PlayBlack ? Color::Black : Color::White);
    ClearSelection();
    Save();
}

void App::SetLevel(int level)
{
    m_level = std::clamp(level, 1, 5);
    if (m_computerThinking)
        StopComputer(); // starts again at the new level
    Save();
}

void App::SetVariant(int variant)
{
    const auto chosen = static_cast<Variant>((variant % draughts::VariantCount + draughts::VariantCount) % draughts::VariantCount);
    if (chosen == m_game.GetVariant())
        return;
    // Other rules make another game.
    StopComputer();
    m_computer.NewGame();
    m_game.Reset(chosen, draughts::Position::Start(chosen));
    m_view->Rebuild(m_game, true);
    m_camera.Reset(m_mode == PlayBlack ? Color::Black : Color::White);
    m_announcedEnd = false;
    AfterPositionChange();
    ShowToast(std::string(T(Str::NewRules)) + std::string(VariantName(chosen)));
}

void App::SetTheme(int theme)
{
    m_theme = (theme % 3 + 3) % 3;
    m_renderer->SetTheme(Themes()[m_theme]);
    Save();
}

void App::RequestHint()
{
    if (!HumanToMove() || m_hintThinking)
        return;
    m_hint.reset();
    m_hinter.Start(m_game.Current(), m_game.GetVariant(), m_game.RepetitionHistory(), {64, 1200, 0, 0});
    m_hintThinking = true;
}

void App::AfterPositionChange()
{
    ClearSelection();
    m_hint.reset();
    m_followMoves = true;
    Save();
}

void App::ClearSelection()
{
    m_selected = -1;
    m_selectedMoves.clear();
    m_route.clear();
    m_hoverSquare = -1;
}

void App::Select(int square)
{
    m_selected = square;
    m_route.clear();
    m_selectedMoves.clear();
    for (const draughts::Move& move : m_game.LegalMoves())
        if (move.from == square)
            m_selectedMoves.push_back(move);
}

std::vector<draughts::Move> App::Candidates() const
{
    std::vector<draughts::Move> candidates;
    for (const draughts::Move& move : m_selectedMoves) {
        bool follows = static_cast<int>(m_route.size()) < move.steps;
        for (std::size_t i = 0; follows && i < m_route.size(); ++i)
            follows = move.path[i] == m_route[i];
        if (follows)
            candidates.push_back(move);
    }
    return candidates;
}

bool App::IsTarget(int square) const
{
    if (square < 0)
        return false;
    return std::ranges::any_of(Candidates(), [&](const draughts::Move& move) { return move.To() == square || move.path[m_route.size()] == square; });
}

std::uint32_t App::ThreatenedBy(int square) const
{
    if (square < 0)
        return 0;
    std::uint32_t taken = 0;
    for (const draughts::Move& move : Candidates()) {
        if (move.To() == square) {
            taken |= move.captured;
        }
        else if (move.path[m_route.size()] == square && move.IsCapture()) {
            for (std::size_t i = 0; i <= m_route.size(); ++i)
                taken |= draughts::Bit(move.taken[i]);
        }
    }
    return taken;
}

void App::ChooseSquare(int square)
{
    const auto candidates = Candidates();
    std::vector<draughts::Move> ending;
    for (const draughts::Move& move : candidates)
        if (move.To() == square)
            ending.push_back(move);
    if (ending.size() == 1) {
        PlayMove(ending.front());
        return;
    }
    if (ending.size() > 1) {
        // Different captures end here: the player shows the way, square by square.
        ShowToast(std::string(T(Str::ChooseRoute)));
        if (m_selected >= 0)
            m_view->Release(m_selected);
        return;
    }
    if (std::ranges::any_of(candidates, [&](const draughts::Move& move) { return move.path[m_route.size()] == square; }))
        m_route.push_back(square);
    if (m_selected >= 0)
        m_view->Release(m_selected);
}

void App::CopyGame(bool position)
{
    const std::wstring text = win32::Widen(position ? m_game.Current().ToFen(m_game.GetVariant()) : m_game.ToPdn(PlayerName(Color::White), PlayerName(Color::Black)));
    if (!OpenClipboard(m_window))
        return;
    EmptyClipboard();
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        }
        else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    ShowToast(std::string(T(position ? Str::FenCopied : Str::PdnCopied)));
}

void App::PasteGame()
{
    std::string text;
    if (OpenClipboard(m_window)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* characters = static_cast<const wchar_t*>(GlobalLock(data))) {
                text = win32::Narrow(characters);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }
    auto game = draughts::Game::Parse(text, m_game.GetVariant());
    if (!game) {
        ShowToast(std::string(T(Str::NothingToPaste)));
        return;
    }
    StopComputer();
    m_game = std::move(*game);
    m_view->Rebuild(m_game, true);
    m_announcedEnd = m_game.Status().Over();
    AfterPositionChange();
    ShowToast(std::string(T(Str::GameLoaded)));
}

void App::Save()
{
    m_settings.theme = m_theme;
    m_settings.level = m_level;
    m_settings.mode = m_mode;
    m_settings.variant = std::to_underlying(m_game.GetVariant());
    m_settings.sound = m_sound.enabled;
    m_settings.panel = m_panelVisible;
    m_settings.language = m_language;
    m_settings.game = m_game.Serialize();
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (m_fullScreen)
        placement = m_placement;
    else if (m_window)
        GetWindowPlacement(m_window, &placement);
    if (placement.rcNormalPosition.right > placement.rcNormalPosition.left) {
        m_settings.hasWindow = true;
        m_settings.window = placement.rcNormalPosition;
        m_settings.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
    }
    m_settings.Save();
}

void App::ToggleFullScreen()
{
    const LONG style = GetWindowLongW(m_window, GWL_STYLE);
    if (!m_fullScreen) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        m_placement.length = sizeof(m_placement);
        if (GetWindowPlacement(m_window, &m_placement) && GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            SetWindowLongW(m_window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            const RECT& r = monitor.rcMonitor;
            SetWindowPos(m_window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            m_fullScreen = true;
        }
    }
    else {
        SetWindowLongW(m_window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(m_window, &m_placement);
        SetWindowPos(m_window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_fullScreen = false;
    }
}

// Input --------------------------------------------------------------------------------------------

Vec3 App::PointerOnPlane(float height) const
{
    Vec3 origin, direction;
    m_camera.Ray(m_pointer.x, m_pointer.y, static_cast<float>(m_width), static_cast<float>(m_height), origin, direction);
    if (std::abs(direction.y) < 1e-5f)
        return origin;
    const float t = (height - origin.y) / direction.y;
    return origin + direction * std::max(t, 0.0f);
}

int App::SquareUnderPointer() const
{
    return board::SquareAt(PointerOnPlane(0));
}

void App::HandleBoardPointer()
{
    const bool blocked = m_pointer.consumed || m_helpVisible;
    const float scale = Scale();
    auto pointedSquare = [&] {
        Vec3 origin, direction;
        m_camera.Ray(m_pointer.x, m_pointer.y, static_cast<float>(m_width), static_cast<float>(m_height), origin, direction);
        const int piece = m_view->Pick(origin, direction);
        return piece >= 0 ? piece : SquareUnderPointer();
    };
    const auto legal = HumanToMove() ? m_game.LegalMoves() : std::vector<draughts::Move>{};
    auto movable = [&](int square) {
        return square >= 0 && std::ranges::any_of(legal, [square](const draughts::Move& move) { return move.from == square; });
    };
    auto ownPiece = [&](int square) {
        const auto piece = square >= 0 && HumanToMove() ? m_game.Current().At(square) : std::nullopt;
        return piece && piece->color == m_game.Current().SideToMove();
    };

    // What the pointer is over.
    const bool insideWindow = m_pointer.x >= 0 && m_pointer.y >= 0 && m_pointer.x < static_cast<float>(m_width) && m_pointer.y < static_cast<float>(m_height);
    if (m_gesture == Gesture::None || m_gesture == Gesture::DragPiece) {
        const int square = insideWindow && !blocked ? pointedSquare() : -1;
        const int target = m_gesture == Gesture::DragPiece ? SquareUnderPointer() : square;
        m_hoverPiece = m_gesture == Gesture::None && movable(square) ? square : -1;
        m_hoverSquare = IsTarget(target) ? target : -1;
    }
    else {
        m_hoverPiece = m_hoverSquare = -1;
    }
    if (m_hoverPiece >= 0 || m_hoverSquare >= 0 || m_gesture == Gesture::DragPiece)
        m_cursorHand = true;

    if (!blocked && m_pointer.leftPressed) {
        const int square = pointedSquare();
        m_pressX = m_dragX = m_pointer.x;
        m_pressY = m_dragY = m_pointer.y;
        if (m_selected >= 0 && IsTarget(square)) {
            ChooseSquare(square);
        }
        else if (movable(square)) {
            if (square != m_selected)
                Select(square);
            m_gesture = Gesture::PressPiece;
            m_pressSquare = square;
        }
        else {
            if (ownPiece(square) && m_game.Current().HasCapture(m_game.GetRules()))
                ShowToast(std::string(T(Str::TakingIsCompulsory)));
            m_gesture = Gesture::TurnCamera;
            m_pressSquare = -1;
        }
    }
    if (!m_pointer.consumed && m_pointer.rightPressed && m_gesture == Gesture::None) {
        m_gesture = Gesture::TurnCamera;
        m_pressSquare = -1;
        m_pressX = m_dragX = m_pointer.x;
        m_pressY = m_dragY = m_pointer.y;
    }

    const float travel = std::abs(m_pointer.x - m_pressX) + std::abs(m_pointer.y - m_pressY);
    if (m_gesture == Gesture::PressPiece && travel > 6 * scale && m_pointer.leftDown)
        m_gesture = Gesture::DragPiece;
    if (m_gesture == Gesture::DragPiece)
        m_view->Hold(m_pressSquare, PointerOnPlane(0.3f) + Vec3{0, 0.05f, 0});
    if (m_gesture == Gesture::TurnCamera) {
        m_camera.Rotate(-(m_pointer.x - m_dragX) * 0.0055f / scale, (m_pointer.y - m_dragY) * 0.0045f / scale);
        m_dragX = m_pointer.x;
        m_dragY = m_pointer.y;
    }

    if (m_pointer.leftReleased || m_pointer.rightReleased) {
        if (m_gesture == Gesture::DragPiece && m_pointer.leftReleased) {
            const int square = SquareUnderPointer();
            if (IsTarget(square))
                ChooseSquare(square);
            else
                m_view->Release(m_pressSquare);
        }
        else if (m_gesture == Gesture::TurnCamera && travel < 4 * scale && m_pointer.leftReleased) {
            ClearSelection(); // a click beside the pieces
        }
        if ((m_pointer.leftReleased && m_gesture != Gesture::None && !(m_gesture == Gesture::TurnCamera && m_pointer.rightDown)) ||
            (m_pointer.rightReleased && m_gesture == Gesture::TurnCamera && !m_pointer.leftDown))
            m_gesture = Gesture::None;
    }

    if (!m_pointer.consumed && m_pointer.wheel != 0)
        m_camera.Zoom(m_pointer.wheel);
}

void App::HandleKey(WPARAM key)
{
    const bool control = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;

    if (m_helpVisible && (key == VK_ESCAPE || key == VK_F1)) {
        m_helpVisible = false;
        return;
    }

    if (control) {
        switch (key) {
        case 'Z': shift ? Redo() : Undo(); return;
        case 'Y': Redo(); return;
        case 'C': CopyGame(shift); return;
        case 'V': PasteGame(); return;
        case 'N': NewGame(); return;
        default: return;
        }
    }

    switch (key) {
    case VK_BACK:
    case VK_LEFT: Undo(); break;
    case VK_RIGHT: Redo(); break;
    case 'N': NewGame(); break;
    case 'H': RequestHint(); break;
    case 'F': m_camera.Flip(); break;
    case 'V': m_camera.ToggleTopView(); break;
    case VK_HOME: m_camera.Reset(m_mode == PlayBlack ? Color::Black : Color::White); break;
    case 'M': SetMode(m_mode + 1); break;
    case 'R': SetVariant(std::to_underlying(m_game.GetVariant()) + (shift ? -1 : 1)); break;
    case 'T': SetTheme(m_theme + (shift ? -1 : 1)); break;
    case 'S':
        m_sound.enabled = !m_sound.enabled;
        Save();
        break;
    case 'L':
        m_language = m_language == Language::English ? Language::Russian : Language::English;
        Save();
        break;
    case VK_TAB:
        m_panelVisible = !m_panelVisible;
        Save();
        break;
    case VK_F1: m_helpVisible = !m_helpVisible; break;
    case VK_F11: ToggleFullScreen(); break;
    case VK_ESCAPE:
        if (m_fullScreen)
            ToggleFullScreen();
        else if (!m_route.empty())
            m_route.pop_back(); // a step back along the route being chosen
        else
            ClearSelection();
        break;
    default:
        if (key >= '1' && key <= '5')
            SetLevel(static_cast<int>(key - '0'));
        else if (key >= VK_NUMPAD1 && key <= VK_NUMPAD5)
            SetLevel(static_cast<int>(key - VK_NUMPAD0));
        break;
    }
}

// The interface ------------------------------------------------------------------------------------

Rect App::PanelRect() const
{
    const float scale = Scale();
    const float width = PanelWidth * scale, margin = PanelMargin * scale;
    return {static_cast<float>(m_width) - (width + margin) * m_panelSlide, margin, width, static_cast<float>(m_height) - 2 * margin};
}

Rect App::FreeArea() const
{
    const Rect panel = PanelRect();
    return {0, 0, std::min(panel.x, static_cast<float>(m_width)), static_cast<float>(m_height)};
}

Rect App::HelpCard() const
{
    const float scale = Scale();
    const float width = 680 * scale, height = 24 * 27 * scale + 2 * 28 * scale + 52 * scale + 12 * scale;
    const Rect free = FreeArea();
    return {free.x + (free.width - width) / 2, std::max(10 * scale, (static_cast<float>(m_height) - height) / 2), width, height};
}

namespace {

Str ResultTitle(draughts::GameResult result)
{
    return result == draughts::GameResult::WhiteWins ? Str::WhiteWins : result == draughts::GameResult::BlackWins ? Str::BlackWins : Str::Draw;
}

Str ReasonText(draughts::GameStatus status, Variant variant)
{
    switch (status.reason) {
    case draughts::EndReason::NoPieces: return variant == Variant::Giveaway ? Str::NoMovesGiveaway : Str::NoPieces;
    case draughts::EndReason::NoMoves: return variant == Variant::Giveaway ? Str::NoMovesGiveaway : Str::NoMoves;
    case draughts::EndReason::Repetition: return Str::ByRepetition;
    case draughts::EndReason::QuietMoves: return Str::ByQuietMoves;
    default: return Str::ByLoneKings;
    }
}

} // namespace

Rect App::BannerCard() const
{
    const float scale = Scale();
    const std::string_view title = T(ResultTitle(m_game.Status().result));
    const float width = std::max(m_painter->Measure(FontId::Display, title) + 80 * scale, 320 * scale);
    const Rect free = FreeArea();
    return {free.x + (free.width - width) / 2, 26 * scale - (1 - m_bannerFade) * 20 * scale, width, 92 * scale};
}

std::vector<render::FrostedPanel> App::FrostedPanels() const
{
    std::vector<render::FrostedPanel> panels;
    const float scale = Scale();
    auto add = [&](Rect rect, float radius, float opacity) { panels.push_back({rect.x, rect.y, rect.width, rect.height, radius * scale, opacity}); };
    if (m_panelSlide > 0.001f)
        add(PanelRect(), 18, 1);
    if (m_bannerFade > 0.001f)
        add(BannerCard(), 18, m_bannerFade);
    if (m_helpVisible)
        add(HelpCard(), 20, 1);
    return panels;
}

void App::ShowToast(std::string text)
{
    if (m_toasts.size() >= 3)
        m_toasts.erase(m_toasts.begin());
    m_toasts.push_back({std::move(text), 0});
}

std::string App::PlayerName(Color side) const
{
    if (m_mode == TwoPlayers)
        return std::string(T(Str::Player));
    if (IsHuman(side))
        return std::string(T(Str::You));
    return std::format("{} · {}", T(Str::Computer), T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)));
}

std::string_view App::VariantName(Variant variant) const
{
    return T(variant == Variant::English ? Str::RulesEnglish : variant == Variant::Giveaway ? Str::RulesGiveaway : Str::RulesRussian);
}

bool App::Button(int id, Rect rect, std::string_view label, bool enabled, bool accent)
{
    const float scale = Scale();
    const bool modal = m_helpVisible;
    const bool interactive = enabled && (!modal || id > ModalWidgets);
    const bool hover = interactive && rect.Contains(m_pointer.x, m_pointer.y);
    if (hover)
        m_cursorHand = true;
    if (hover && m_pointer.leftPressed) {
        m_activeWidget = id;
        m_pointer.consumed = true;
    }
    bool clicked = false;
    if (m_pointer.leftReleased && m_activeWidget == id) {
        clicked = hover;
        m_activeWidget = 0;
        m_pointer.consumed = true;
    }
    const bool pressed = hover && m_activeWidget == id && m_pointer.leftDown;
    ui::Color fill = !enabled ? Rgba(255, 255, 255, 7) : pressed ? Rgba(255, 255, 255, 52) : hover ? Rgba(255, 255, 255, 34) : Rgba(255, 255, 255, 17);
    if (accent && enabled)
        fill = ui::Mix(fill, ui::FromLinear(Themes()[m_theme].accent, static_cast<float>(fill.a) / 255.0f * 2.2f), 0.6f);
    m_painter->Fill(rect, 9 * scale, fill);
    m_painter->TextCentered(FontId::Body, rect, label, enabled ? TextColor : ui::Fade(TextColor, 0.35f));
    if (clicked)
        m_sound.Play(SoundBank::Effect::Click);
    return clicked;
}

int App::Selector(int id, Rect rect, std::string_view label, std::string_view value)
{
    const float scale = Scale();
    m_painter->Text(FontId::Body, rect.x, rect.y + (rect.height - m_painter->LineHeight(FontId::Body)) / 2, label, MutedColor);
    const float arrow = rect.height;
    const Rect right{rect.Right() - arrow, rect.y, arrow, rect.height};
    const Rect valueRect{rect.x + rect.width * 0.36f, rect.y, rect.width * 0.64f - arrow - 4 * scale, rect.height};
    const Rect left{valueRect.x - arrow - 4 * scale, rect.y, arrow, rect.height};
    m_painter->Fill({left.Right() + 2 * scale, rect.y, right.x - left.Right() - 4 * scale, rect.height}, 9 * scale, Rgba(255, 255, 255, 9));
    m_painter->TextCentered(FontId::Body, {left.Right(), rect.y, right.x - left.Right(), rect.height}, value, TextColor);
    int change = 0;
    if (Button(id, left, ""))
        change = -1;
    if (Button(id + 1, right, ""))
        change = 1;
    const float size = 5 * scale;
    m_painter->Arrow(left.x + left.width / 2, left.y + left.height / 2, size * 0.8f, size, -1, TextColor);
    m_painter->Arrow(right.x + right.width / 2, right.y + right.height / 2, size * 0.8f, size, 1, TextColor);
    return change;
}

void App::DrawInterface()
{
    DrawPanel();
    DrawBanner();
    DrawToasts();
    if (m_helpVisible)
        DrawHelp();
}

void App::DrawStatus(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const draughts::GameStatus status = m_game.Status();
    const Color side = m_game.Current().SideToMove();
    const ui::Color accent = ui::FromLinear(Themes()[m_theme].accent);
    p.Fill(area, 12 * scale, Rgba(255, 255, 255, 14));

    // A disc of the side to move.
    const float discX = area.x + 22 * scale, discY = area.y + area.height / 2;
    p.Disc(discX, discY, 10 * scale, Rgba(120, 125, 140));
    p.Disc(discX, discY, 9 * scale, side == Color::White ? Rgba(238, 232, 220) : Rgba(22, 22, 26));

    std::string title, detail;
    ui::Color titleColor = TextColor, detailColor = MutedColor;
    if (status.Over()) {
        title = T(ResultTitle(status.result));
        detail = T(ReasonText(status, m_game.GetVariant()));
        titleColor = accent;
    }
    else {
        title = T(side == Color::White ? Str::WhiteToMove : Str::BlackToMove);
        if (m_computerThinking || m_computerResult) {
            detail = T(Str::Thinking);
        }
        else if (HumanToMove() && m_game.Current().HasCapture(m_game.GetRules())) {
            detail = T(Str::MustTake);
            detailColor = ui::FromLinear(Themes()[m_theme].accentWarm);
        }
        else {
            detail = PlayerName(side);
        }
    }
    const float textX = area.x + 44 * scale;
    const float lineHeight = p.LineHeight(FontId::Heading);
    p.Text(FontId::Heading, textX, area.y + area.height / 2 - lineHeight + 3 * scale, title, titleColor);
    p.Text(FontId::Body, textX, area.y + area.height / 2 + 2 * scale, detail, detailColor);
    if (m_computerThinking || m_computerResult || m_hintThinking)
        p.Spinner(area.Right() - 22 * scale, area.y + area.height / 2, 8 * scale, 2.2f * scale, m_time * 5.0f, accent);
}

void App::DrawMoveList(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill(area, 12 * scale, Rgba(0, 0, 0, 40));
    const auto moves = m_game.Moves();
    if (moves.empty()) {
        p.TextCentered(FontId::Body, area, T(Str::NoMovesYet), MutedColor);
        return;
    }

    const float rowHeight = std::round(25 * scale);
    const Rect inner = area.Inset(8 * scale);
    const int visible = std::max(1, static_cast<int>(inner.height / rowHeight));
    // Each row starts with the side that moves first in these rules.
    const int offset = m_game.Start().SideToMove() != m_game.GetRules().first ? 1 : 0;
    const int rows = (static_cast<int>(moves.size()) + offset + 1) / 2;
    const float maxScroll = static_cast<float>(std::max(0, rows - visible));

    if (area.Contains(m_pointer.x, m_pointer.y) && m_pointer.wheel != 0 && !m_helpVisible) {
        m_moveScroll = std::clamp(m_moveScroll - m_pointer.wheel * 3, 0.0f, maxScroll);
        m_followMoves = m_moveScroll >= maxScroll;
        m_pointer.wheel = 0;
    }
    if (m_followMoves)
        m_moveScroll = maxScroll;
    m_moveScroll = std::clamp(m_moveScroll, 0.0f, maxScroll);

    const int first = static_cast<int>(m_moveScroll);
    const float numberWidth = 40 * scale, columnWidth = (inner.width - numberWidth) / 2;
    const ui::Color accent = ui::FromLinear(Themes()[m_theme].accent);
    for (int row = first; row < std::min(rows, first + visible); ++row) {
        const float y = inner.y + static_cast<float>(row - first) * rowHeight;
        const std::string number = std::format("{}.", row + 1);
        p.Text(FontId::Body, inner.x + numberWidth - 10 * scale - p.Measure(FontId::Body, number), y + 3 * scale, number, MutedColor);
        for (int column = 0; column < 2; ++column) {
            const int ply = row * 2 + column - offset;
            if (ply < 0 || ply >= static_cast<int>(moves.size()))
                continue;
            const float x = inner.x + numberWidth + static_cast<float>(column) * columnWidth;
            if (ply == static_cast<int>(moves.size()) - 1)
                p.Fill({x - 6 * scale, y + 1 * scale, columnWidth - 6 * scale, rowHeight - 2 * scale}, 7 * scale, ui::Fade(accent, 0.28f));
            // A long capture that does not fit shows where it began and ended.
            std::string text = moves[static_cast<std::size_t>(ply)].notation;
            if (p.Measure(FontId::Body, text) > columnWidth - 12 * scale) {
                const std::size_t start = text.find_first_of("-:x"), end = text.find_last_of("-:x");
                text = text.substr(0, start + 1) + "…" + text.substr(end);
            }
            p.Text(FontId::Body, x, y + 3 * scale, text, TextColor);
        }
    }
    if (maxScroll > 0) {
        // A thin scroll bar.
        const float barHeight = inner.height * static_cast<float>(visible) / static_cast<float>(rows);
        const float barY = inner.y + (inner.height - barHeight) * (m_moveScroll / maxScroll);
        p.Fill({area.Right() - 5 * scale, barY, 3 * scale, barHeight}, 1.5f * scale, Rgba(255, 255, 255, 50));
    }
}

void App::DrawPanel()
{
    if (m_panelSlide <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect panel = PanelRect();
    if (panel.Contains(m_pointer.x, m_pointer.y) && (m_pointer.leftPressed || m_pointer.rightPressed))
        m_pointer.consumed = true;
    p.Shadow(panel, 22 * scale, Rgba(0, 0, 0, 70));
    p.Fill(panel, 18 * scale, Rgba(16, 18, 26, 120));
    p.Outline(panel, 18 * scale, Rgba(255, 255, 255, 24));

    const float pad = 20 * scale;
    const float x = panel.x + pad, width = panel.width - 2 * pad;
    float y = panel.y + pad;

    p.Text(FontId::Heading, x, y, T(Str::Title), TextColor);
    if (Button(HelpButton, {panel.Right() - pad - 30 * scale, y - 2 * scale, 30 * scale, 30 * scale}, "?"))
        m_helpVisible = !m_helpVisible;
    y += 44 * scale;

    DrawStatus({x, y, width, 62 * scale});
    y += 62 * scale + 14 * scale;

    // The players, with the men and kings each has left.
    const draughts::Position& position = m_game.Current();
    for (Color side : {Color::White, Color::Black}) {
        const float rowHeight = 28 * scale;
        const float textY = y + (rowHeight - p.LineHeight(FontId::Body)) / 2;
        p.Disc(x + 9 * scale, y + rowHeight / 2, 7 * scale, Rgba(120, 125, 140));
        p.Disc(x + 9 * scale, y + rowHeight / 2, 6 * scale, side == Color::White ? Rgba(238, 232, 220) : Rgba(22, 22, 26));
        p.Text(FontId::Body, x + 26 * scale, textY, std::format("{}  ·  {}", T(side == Color::White ? Str::White : Str::Black), PlayerName(side)), TextColor);
        // The men and the kings left, each count after a little picture of the piece.
        const ui::Color rim = Rgba(120, 125, 140), fill = side == Color::White ? Rgba(238, 232, 220) : Rgba(22, 22, 26);
        float right = x + width;
        auto count = [&](int number, bool king) {
            const std::string text = std::to_string(number);
            const float textWidth = p.Measure(FontId::Body, text);
            p.Text(FontId::Body, right - textWidth, textY, text, MutedColor);
            const float iconX = right - textWidth - 11 * scale, iconY = y + rowHeight / 2;
            for (int disc = 0; disc < (king ? 2 : 1); ++disc) {
                const float offset = king ? (disc == 0 ? 2.5f : -2.5f) * scale : 0.0f;
                p.Disc(iconX, iconY + offset, 6 * scale, rim);
                p.Disc(iconX, iconY + offset, 5 * scale, fill);
            }
            right = iconX - 6 * scale - 12 * scale;
        };
        if (const int kings = std::popcount(position.Kings(side)); kings > 0)
            count(kings, true);
        count(std::popcount(position.Men(side)), false);
        y += rowHeight;
    }
    y += 12 * scale;

    // From the bottom up: the footer, the settings and the buttons; the moves take the space between.
    float bottom = panel.Bottom() - pad;
    const float footerHeight = p.LineHeight(FontId::Body);
    p.TextCentered(FontId::Body, {x, bottom - footerHeight, width, footerHeight}, T(Str::Footer), ui::Fade(MutedColor, 0.8f));
    bottom -= footerHeight + 14 * scale;

    const float rowHeight = 34 * scale, gap = 8 * scale;
    const float half = (width - gap) / 2;
    bottom -= rowHeight;
    if (Button(SoundButton, {x, bottom, half, rowHeight}, std::format("{}: {}", T(Str::Sound), T(m_sound.enabled ? Str::On : Str::Off)))) {
        m_sound.enabled = !m_sound.enabled;
        Save();
    }
    if (Button(LanguageButton, {x + half + gap, bottom, half, rowHeight}, T(Str::LanguageName))) {
        m_language = m_language == Language::English ? Language::Russian : Language::English;
        Save();
    }
    bottom -= gap;

    const std::array<std::string_view, 4> modes{T(Str::ModeWhite), T(Str::ModeBlack), T(Str::ModeTwoPlayers), T(Str::ModeDemo)};
    const Theme& theme = Themes()[m_theme];
    struct Choice {
        int id;
        Str label;
        std::string value;
    };
    const std::array<Choice, 4> choices{{
        {ThemeSelector, Str::Theme, std::string(m_language == Language::Russian ? theme.nameRussian : theme.nameEnglish)},
        {RulesSelector, Str::Rules, std::string(VariantName(m_game.GetVariant()))},
        {LevelSelector, Str::Level, std::format("{} · {}", m_level, T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)))},
        {ModeSelector, Str::Mode, std::string(modes[static_cast<std::size_t>(m_mode)])},
    }};
    for (const Choice& choice : choices) {
        bottom -= rowHeight;
        if (const int change = Selector(choice.id, {x, bottom, width, rowHeight}, T(choice.label), choice.value); change != 0) {
            if (choice.id == ThemeSelector)
                SetTheme(m_theme + change);
            else if (choice.id == RulesSelector)
                SetVariant(std::to_underlying(m_game.GetVariant()) + change);
            else if (choice.id == LevelSelector)
                SetLevel(std::clamp(m_level + change, 1, 5));
            else
                SetMode(m_mode + change);
        }
        bottom -= 6 * scale;
    }
    bottom -= 8 * scale;

    const float buttonHeight = 38 * scale;
    bottom -= buttonHeight;
    if (Button(NewGameButton, {x, bottom, half, buttonHeight}, T(Str::NewGame)))
        NewGame();
    if (Button(FlipButton, {x + half + gap, bottom, half, buttonHeight}, T(Str::Flip)))
        m_camera.Flip();
    bottom -= gap + buttonHeight;
    const float third = (width - 2 * gap) / 3;
    if (Button(UndoButton, {x, bottom, third, buttonHeight}, T(Str::Undo), m_game.CanUndo()))
        Undo();
    if (Button(RedoButton, {x + third + gap, bottom, third, buttonHeight}, T(Str::Redo), m_game.CanRedo()))
        Redo();
    if (Button(HintButton, {x + 2 * (third + gap), bottom, third, buttonHeight}, T(Str::Hint), HumanToMove() && !m_hintThinking, true))
        RequestHint();
    bottom -= 16 * scale;

    p.Text(FontId::Body, x, y, T(Str::Moves), MutedColor);
    y += p.LineHeight(FontId::Body) + 6 * scale;
    if (bottom - y > 40 * scale)
        DrawMoveList({x, y, width, bottom - y});
}

void App::DrawBanner()
{
    if (m_bannerFade <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const draughts::GameStatus status = m_game.Status();
    const Rect card = BannerCard();
    p.Shadow(card, 24 * scale, ui::Fade(Rgba(0, 0, 0, 120), m_bannerFade));
    p.Fill(card, 18 * scale, ui::Fade(Rgba(14, 16, 24, 150), m_bannerFade));
    p.Outline(card, 18 * scale, ui::Fade(ui::FromLinear(Themes()[m_theme].accent), 0.6f * m_bannerFade));
    p.TextCentered(FontId::Display, {card.x, card.y + 8 * scale, card.width, 48 * scale}, T(ResultTitle(status.result)), ui::Fade(TextColor, m_bannerFade));
    p.TextCentered(FontId::Body, {card.x, card.y + 54 * scale, card.width, 26 * scale}, T(ReasonText(status, m_game.GetVariant())),
                   ui::Fade(MutedColor, m_bannerFade));
}

void App::DrawToasts()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect free = FreeArea();
    float y = static_cast<float>(m_height) - 40 * scale;
    for (auto toast = m_toasts.rbegin(); toast != m_toasts.rend(); ++toast) {
        const float fade = std::min({1.0f, toast->age * 6, (2.8f - toast->age) * 3});
        const float width = p.Measure(FontId::Body, toast->text) + 40 * scale, height = 36 * scale;
        const Rect pill{free.x + (free.width - width) / 2, y - height, width, height};
        p.Shadow(pill, 14 * scale, ui::Fade(Rgba(0, 0, 0, 110), fade));
        p.Fill(pill, height / 2, ui::Fade(Rgba(20, 22, 30, 225), fade));
        p.TextCentered(FontId::Body, pill, toast->text, ui::Fade(TextColor, fade));
        y -= height + 8 * scale;
    }
}

void App::DrawHelp()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));

    struct Line {
        std::string_view keys;
        Str action;
    };
    const std::array<Line, 21> lines{{
        {T(Str::KeyClick), Str::HelpClick},
        {T(Str::KeyDrag), Str::HelpDrag},
        {T(Str::KeyRightDrag), Str::HelpRotate},
        {T(Str::KeyWheel), Str::HelpZoom},
        {"Ctrl+Z  ←  Backspace", Str::HelpUndo},
        {"Ctrl+Y  →", Str::HelpRedo},
        {"H", Str::HelpHint},
        {"N", Str::HelpNew},
        {"F", Str::HelpFlip},
        {"V", Str::HelpTopView},
        {"1 – 5", Str::HelpLevel},
        {"M", Str::HelpMode},
        {"R", Str::HelpRules},
        {"T", Str::HelpTheme},
        {"S", Str::HelpSound},
        {"L", Str::HelpLanguage},
        {"Tab", Str::HelpPanel},
        {"F11  Alt+Enter", Str::HelpFullScreen},
        {"Ctrl+C", Str::HelpCopy},
        {"Ctrl+V", Str::HelpPaste},
        {"Esc  F1", Str::HelpClose},
    }};
    const float rowHeight = 27 * scale, keysWidth = 200 * scale, pad = 28 * scale;
    const Rect card = HelpCard();
    const ui::Color accent = ui::FromLinear(Themes()[m_theme].accent);
    p.Shadow(card, 26 * scale, Rgba(0, 0, 0, 140));
    p.Fill(card, 20 * scale, Rgba(18, 20, 28, 175));
    p.Outline(card, 20 * scale, Rgba(255, 255, 255, 30));
    p.Text(FontId::Heading, card.x + pad, card.y + pad - 4 * scale, T(Str::Controls), TextColor);
    float y = card.y + pad + 44 * scale;
    for (const Line& line : lines) {
        p.Text(FontId::Body, card.x + pad, y, line.keys, accent);
        p.Text(FontId::Body, card.x + pad + keysWidth, y, T(line.action), TextColor);
        y += rowHeight;
    }
    // The rules being played, in a sentence.
    const Variant variant = m_game.GetVariant();
    const Str note = variant == Variant::English ? Str::RulesNoteEnglish : variant == Variant::Giveaway ? Str::RulesNoteGiveaway : Str::RulesNoteRussian;
    y += 12 * scale;
    p.Text(FontId::Body, card.x + pad, y, std::format("{}: {}", T(Str::Rules), VariantName(variant)), accent);
    // Wrapped to the card's width.
    std::string line;
    std::string_view words = T(note);
    while (!words.empty()) {
        const std::size_t space = std::min(words.find(' '), words.size());
        const std::string_view word = words.substr(0, space);
        words.remove_prefix(std::min(space + 1, words.size()));
        const std::string longer = line.empty() ? std::string(word) : line + ' ' + std::string(word);
        if (!line.empty() && p.Measure(FontId::Body, longer) > card.width - 2 * pad) {
            y += rowHeight;
            p.Text(FontId::Body, card.x + pad, y, line, MutedColor);
            line = word;
        }
        else {
            line = longer;
        }
    }
    p.Text(FontId::Body, card.x + pad, y + rowHeight, line, MutedColor);
    if (Button(HelpClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") ||
        (m_pointer.leftPressed && !card.Contains(m_pointer.x, m_pointer.y)))
        m_helpVisible = false;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
}
