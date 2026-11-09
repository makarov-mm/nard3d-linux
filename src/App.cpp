#include "App.h"

#include "BoardModel.h"
#include "Desktop.h"
#include "Themes.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

using namespace math;
using nardy::Step;
using ui::FontId;
using ui::Rect;
using ui::Rgba;

namespace {

constexpr float PanelWidth = 352, PanelMargin = 16;
constexpr float MinThink = 0.5f;   // seconds the computer takes over a move, at least
constexpr float StepPause = 0.45f; // between the computer's steps
constexpr float TurnPause = 0.55f; // after the last step, before the turn passes
constexpr float PassPause = 1.6f;  // when nothing can be played
constexpr float DragStart = 6;     // pixels the pointer travels before a press becomes a drag

enum Widget {
    HelpButton = 1,
    AboutButton,
    RollButton,
    HintButton,
    UndoButton,
    NewGameButton,
    AutoRollButton,
    CameraButton,
    SoundButton,
    LanguageButton,
    GameSelector,
    ModeSelector = GameSelector + 2,
    LevelSelector = ModeSelector + 2,
    ThemeSelector = LevelSelector + 2,
    ModalWidgets = 100, // the widgets of the cards over the game
    HelpClose,
    AboutClose,
    AboutLinks = 110,
};

constexpr ui::Color TextColor = Rgba(236, 239, 245);
constexpr ui::Color MutedColor = Rgba(150, 158, 176);
constexpr ui::Color NoteColor = Rgba(255, 200, 130);

// The dice of a roll in the order they are played: two, or four of a double.
std::vector<int> DiceOf(nardy::Roll roll)
{
    if (roll.Double())
        return {roll.first, roll.first, roll.first, roll.first};
    return {roll.first, roll.second};
}

// Whether the move's steps begin with `prefix`.
bool StartsWith(const nardy::Move& move, const std::vector<Step>& prefix)
{
    if (move.count < static_cast<int>(prefix.size()))
        return false;
    return std::equal(prefix.begin(), prefix.end(), move.steps.begin());
}

// The letter of a key for the shortcuts: by its label in Latin layouts (Z on a German keyboard is where it is
// marked), and by its place on a US keyboard in the others, such as the Russian one, as Windows does.
char ShortcutLetter(const SDL_Keysym& key)
{
    if (key.sym >= SDLK_a && key.sym <= SDLK_z)
        return static_cast<char>(key.sym);
    if (key.sym >= 0x80 && key.scancode >= SDL_SCANCODE_A && key.scancode <= SDL_SCANCODE_Z)
        return static_cast<char>('a' + (key.scancode - SDL_SCANCODE_A));
    return 0;
}

// The computer level of a digit key, on the main keys or the keypad, or 0.
int LevelKey(const SDL_Keysym& key)
{
    if (key.scancode >= SDL_SCANCODE_1 && key.scancode <= SDL_SCANCODE_5)
        return key.scancode - SDL_SCANCODE_1 + 1;
    if (key.scancode >= SDL_SCANCODE_KP_1 && key.scancode <= SDL_SCANCODE_KP_5)
        return key.scancode - SDL_SCANCODE_KP_1 + 1;
    return 0;
}

} // namespace

// Setup --------------------------------------------------------------------------------------------

App::SdlSession::SdlSession()
{
    SDL_SetHint(SDL_HINT_APP_NAME, "3D Nardy");
    // A game in a window rather than a full-screen one: the desktop goes on compositing it, the screen saver may
    // start, and leaving full screen for another window does not minimize it.
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(std::format("The display could not be opened: {}", SDL_GetError()));
}

App::SdlSession::~SdlSession() { SDL_Quit(); }

App::App()
{
    m_settings = Settings::Load();
    m_language = m_settings.language;
    m_theme = m_settings.theme;
    m_level = m_settings.level;
    m_mode = m_settings.mode;
    m_game = m_settings.game;
    m_autoRoll = m_settings.autoRoll;
    m_score = m_settings.score;
    m_sound.SetEnabled(m_settings.sound);
    m_panelVisible = m_settings.panel;
    m_panelSlide = m_panelVisible ? 1.0f : 0.0f;
    m_random.seed((static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
                  static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

    const resources::Image icon = resources::Png("nardy3d.png");
    CreateMainWindow(icon);
    m_context = std::make_unique<gl::Context>(m_window.get());
    render::Program::SetVersion(m_context->Version());
    m_painter = std::make_unique<ui::Painter>();
    m_painter->SetIcon(icon);
    SDL_ShowWindow(m_window.get());
    if (m_settings.maximized)
        SDL_MaximizeWindow(m_window.get());
    UpdateSize();
    BuildFonts();
    ShowLoading();

    m_renderer = std::make_unique<render::Renderer>(m_context->Version());
    m_renderer->SetTheme(Themes()[static_cast<std::size_t>(m_theme)]);
    m_camera.SetMode(static_cast<CameraMode>(m_settings.camera));
    LoadNetworks();
    // The history of the game as far as it was kept, before the game itself: the turn it continues remembers where its
    // lines end, for taking it back.
    std::string_view history = m_settings.history;
    while (!history.empty()) {
        const std::size_t end = std::min(history.find('|'), history.size());
        const std::string_view line = history.substr(0, end);
        if (line.size() > 1 && line[0] >= '0' && line[0] <= '3')
            m_history.push_back({std::string(line.substr(1)), line[0] - '0'});
        history.remove_prefix(std::min(end + 1, history.size()));
    }
    if (!LoadGame(m_settings.match))
        NewGame();
    m_lastFrame = std::chrono::steady_clock::now();
}

App::~App()
{
    StopComputer();
}

void App::CreateMainWindow(const resources::Image& icon)
{
    gl::Context::RequestPixelFormat();
    // Hidden until it has its size, which depends on the desktop's scaling, which is known from a window.
    m_window.reset(SDL_CreateWindow(std::string(T(Str::Title)).c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 800,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN));
    if (!m_window)
        throw std::runtime_error(std::format("The window could not be created: {}", SDL_GetError()));
    SDL_Window* window = m_window.get();
    desktop::SetIcon(window, icon);
    SDL_StopTextInput(); // keys only: no input method in the way
    m_textScale = desktop::TextScale(window);

    // The last size, or most of the screen's work area, centered in it. Room is left for the frame, so that on a small
    // screen with a large scale the window can be seen whole even at its least size.
    const int display = std::max(0, SDL_GetWindowDisplayIndex(window));
    SDL_Rect work{0, 0, 1920, 1080};
    if (SDL_GetDisplayUsableBounds(display, &work) != 0)
        SDL_GetDisplayBounds(display, &work);
    const auto units = [&](int size) { return static_cast<int>(std::lround(static_cast<float>(size) * m_textScale)); };
    const int roomWidth = work.w * 92 / 100, roomHeight = work.h * 92 / 100;
    int width = m_settings.windowWidth, height = m_settings.windowHeight;
    if (width <= 0 || height <= 0) {
        width = std::min(units(1500), roomWidth);
        height = std::min(units(900), roomHeight);
    }
    width = std::min(width, work.w);
    height = std::min(height, work.h);
    SDL_SetWindowMinimumSize(window, std::min(units(960), roomWidth), std::min(units(680), roomHeight));
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, work.x + (work.w - width) / 2, work.y + (work.h - height) / 2);
    m_normalSize = m_previousNormalSize = {width, height};

    m_arrowCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    m_handCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
}

void App::UpdateSize()
{
    SDL_GetWindowSize(m_window.get(), &m_windowWidth, &m_windowHeight);
    SDL_GL_GetDrawableSize(m_window.get(), &m_width, &m_height);
    const float pixelsPerUnit = m_windowWidth > 0 && m_width > 0 ? static_cast<float>(m_width) / static_cast<float>(m_windowWidth) : 1.0f;
    m_scale = std::clamp(m_textScale * pixelsPerUnit, 0.5f, 4.0f);
}

void App::BuildFonts()
{
    const float scale = Scale();
    m_fonts[0] = std::make_unique<text::Font>(15 * scale, text::Weight::Regular);
    m_fonts[1] = std::make_unique<text::Font>(19 * scale, text::Weight::SemiBold);
    m_fonts[2] = std::make_unique<text::Font>(31 * scale, text::Weight::SemiBold);
    m_painter->SetFonts(m_fonts[0].get(), m_fonts[1].get(), m_fonts[2].get());
    m_fontScale = scale;
}

void App::ShowLoading()
{
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, m_width, m_height);
    gl::glClearColor(0.014f, 0.012f, 0.01f, 1);
    gl::glClear(gl::GL_COLOR_BUFFER_BIT);
    m_painter->Begin(m_width, m_height);
    const Rect screen{0, 0, static_cast<float>(m_width), static_cast<float>(m_height)};
    m_painter->TextCentered(FontId::Display, screen, T(Str::Title), TextColor);
    m_painter->Flush();
    m_context->SwapBuffers();
}

// The computer's judgement of positions, one network for each game, built into the program.
void App::LoadNetworks()
{
    constexpr std::array<std::string_view, 2> names{"long.net", "short.net"};
    for (std::size_t i = 0; i < names.size(); ++i)
        if (!m_nets[i].Load(std::as_bytes(resources::Data(names[i]))))
            throw std::runtime_error(std::format("The computer player's network {} built into the program is damaged.", names[i]));
}

int App::Run()
{
    SDL_Event event;
    while (m_running) {
        // Nothing moving and nothing new, or nothing to be seen: wait for input instead of drawing the same picture.
        const bool hidden = (SDL_GetWindowFlags(m_window.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        if (hidden || (!m_dirty && !NeedsAnimation())) {
            if (SDL_WaitEvent(&event))
                HandleEvent(event);
            m_lastFrame = std::chrono::steady_clock::now(); // the idle time is not animation time
        }
        while (SDL_PollEvent(&event))
            HandleEvent(event);
        if (!m_running || hidden || (!m_dirty && !NeedsAnimation()))
            continue;

        m_dirty = false;
        const auto start = std::chrono::steady_clock::now();
        Frame();
        // Swapping the buffers waits for the screen; where it does not, the loop sleeps a little.
        if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2))
            SDL_Delay(1);
    }
    Save();
    return 0;
}

void App::HandleEvent(const SDL_Event& event)
{
    m_dirty = true;
    switch (event.type) {
    case SDL_QUIT: m_running = false; break;
    case SDL_WINDOWEVENT: HandleWindowEvent(event.window); break;
    case SDL_MOUSEMOTION: PointerAt(event.motion.x, event.motion.y); break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        // A click that no frame has seen yet is handled before the next press, so that quick clicks are each heard even
        // when frames are slow: the second of a double-click as such.
        if (event.type == SDL_MOUSEBUTTONDOWN && ((m_pointer.leftPressed && m_pointer.leftReleased) || (m_pointer.rightPressed && m_pointer.rightReleased)))
            Frame();
        PointerAt(event.button.x, event.button.y);
        const bool down = event.type == SDL_MOUSEBUTTONDOWN;
        if (event.button.button == SDL_BUTTON_LEFT) {
            m_pointer.leftDown = down;
            (down ? m_pointer.leftPressed : m_pointer.leftReleased) = true;
            // Every second click in quick succession is a double-click, as Windows counts them.
            if (down)
                m_pointer.doubleClick = event.button.clicks >= 2 && event.button.clicks % 2 == 0;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT) {
            m_pointer.rightDown = down;
            (down ? m_pointer.rightPressed : m_pointer.rightReleased) = true;
        }
        break;
    }
    case SDL_MOUSEWHEEL:
        // Away from the user is positive, in notches; touchpads scroll in fractions of one.
        m_pointer.wheel += event.wheel.preciseY;
        break;
    case SDL_KEYDOWN: HandleKey(event.key); break;
    default: break;
    }
}

void App::HandleWindowEvent(const SDL_WindowEvent& event)
{
    switch (event.event) {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        UpdateSize();
        if (!m_fullScreen && (SDL_GetWindowFlags(m_window.get()) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0) {
            m_previousNormalSize = m_normalSize;
            m_normalSize = {m_windowWidth, m_windowHeight};
            m_normalSizeTime = event.timestamp;
        }
        break;
    case SDL_WINDOWEVENT_MAXIMIZED:
        // The new size may come a moment before the word that the window is maximized: it was not a size of its own.
        if (event.timestamp - m_normalSizeTime < 250)
            m_normalSize = m_previousNormalSize;
        break;
    case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        m_textScale = desktop::TextScale(m_window.get());
        UpdateSize();
        break;
    case SDL_WINDOWEVENT_LEAVE:
        // So that nothing stays highlighted under a pointer that has gone.
        if (!m_pointer.leftDown && !m_pointer.rightDown)
            m_pointer.x = m_pointer.y = -1;
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        // A button let go of in another window is not heard here.
        if (m_pointer.leftDown)
            m_pointer.leftReleased = true;
        if (m_pointer.rightDown)
            m_pointer.rightReleased = true;
        m_pointer.leftDown = m_pointer.rightDown = false;
        break;
    default: break;
    }
}

void App::PointerAt(int windowX, int windowY)
{
    const float pixelsPerUnit = m_windowWidth > 0 ? static_cast<float>(m_width) / static_cast<float>(m_windowWidth) : 1.0f;
    m_pointer.x = static_cast<float>(windowX) * pixelsPerUnit;
    m_pointer.y = static_cast<float>(windowY) * pixelsPerUnit;
}

// The frame ----------------------------------------------------------------------------------------

void App::Frame()
{
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::min(std::chrono::duration<float>(now - m_lastFrame).count(), 0.1f);
    m_lastFrame = now;
    m_time += elapsed;
    if (m_width <= 0 || m_height <= 0)
        return;
    if (m_scale != m_fontScale)
        BuildFonts();
    m_renderer->Resize(m_width, m_height);

    Update(elapsed);
    m_renderer->Render(BuildScene());

    m_cursorHand = false;
    m_painter->Begin(m_width, m_height);
    DrawInterface();
    m_painter->Flush();
    HandleBoardPointer();

    // The widgets are drawn as they were before they heard the click, and a checker carried follows the pointer after
    // the frame is drawn: what they changed shows in the next frame.
    if (m_pointer.leftPressed || m_pointer.leftReleased || m_pointer.rightPressed || m_pointer.rightReleased || m_pointer.x != m_drawnPointer.x ||
        m_pointer.y != m_drawnPointer.y)
        m_dirty = true;
    m_drawnPointer = {m_pointer.x, m_pointer.y};
    m_dialogOpening = false;
    m_pointer.leftPressed = m_pointer.leftReleased = m_pointer.rightPressed = m_pointer.rightReleased = false;
    m_pointer.doubleClick = false;
    m_pointer.wheel = 0;
    m_pointer.consumed = false;
    if (m_cursorHand != m_cursorShownHand) {
        SDL_SetCursor(m_cursorHand ? m_handCursor.get() : m_arrowCursor.get());
        m_cursorShownHand = m_cursorHand;
    }
    m_context->SwapBuffers();
}

void App::Update(float seconds)
{
    const float slideTarget = m_panelVisible ? 1.0f : 0.0f;
    m_panelSlide += (slideTarget - m_panelSlide) * (1 - std::exp(-seconds * 12));
    if (std::abs(slideTarget - m_panelSlide) < 0.001f)
        m_panelSlide = slideTarget;
    m_camera.SetSpace(FreeArea().width / std::max(1.0f, static_cast<float>(m_width)));

    m_stageClock += seconds;
    switch (m_stage) {
    case Stage::Opening:
        if (!m_view.DiceRolling() && m_stageClock > 1.6f)
            FinishOpening();
        break;
    case Stage::ToRoll:
        if ((!IsHuman(m_toPlay) && m_stageClock > 0.45f) || (IsHuman(m_toPlay) && m_autoRoll && m_stageClock > 0.3f))
            RollDice();
        break;
    case Stage::Rolling:
        if (!m_view.DiceRolling())
            BeginMoves();
        break;
    case Stage::Moving:
        if (m_doneClock > 0) {
            m_doneClock -= seconds;
            if (m_doneClock <= 0)
                EndTurn();
        }
        break;
    case Stage::Thinking:
    case Stage::Playing:
        UpdateComputer(seconds);
        break;
    case Stage::Over:
        if (m_mode == Demo && m_stageClock > 6)
            NewGame();
        break;
    }
    UpdateHint();
    m_view.Update(seconds);
    PlaySounds();
    m_camera.Update(seconds);

    for (Toast& toast : m_toasts)
        toast.age += seconds;
    std::erase_if(m_toasts, [](const Toast& toast) { return toast.age > 2.8f; });
    const float bannerTarget = BannerTarget();
    if (m_bannerTime > 0)
        m_bannerTime = std::max(0.0f, m_bannerTime - seconds);
    m_bannerFade += (bannerTarget - m_bannerFade) * (1 - std::exp(-seconds * 7));
    if (std::abs(bannerTarget - m_bannerFade) < 0.002f)
        m_bannerFade = bannerTarget;
}

void App::PlaySounds()
{
    const BoardView::Sounds sounds = m_view.TakeSounds();
    std::uniform_real_distribution<float> vary(0.93f, 1.07f);
    using Effect = SoundBank::Effect;
    for (int i = 0; i < std::min(sounds.landings, 3); ++i)
        m_sound.Play(Effect::Checker, 0.8f, 0, vary(m_random));
    for (int i = 0; i < std::min(sounds.hits, 2); ++i)
        m_sound.Play(Effect::Hit, 0.9f, 0, vary(m_random));
    for (int i = 0; i < std::min(sounds.trays, 3); ++i)
        m_sound.Play(Effect::Tray, 0.8f, 0.4f, vary(m_random));
    for (const float volume : sounds.dice)
        m_sound.Play(Effect::Die, volume, 0, vary(m_random));
}

bool App::NeedsAnimation() const
{
    const bool waiting = m_stage == Stage::Opening || m_stage == Stage::Rolling || m_stage == Stage::Thinking || m_stage == Stage::Playing ||
                         (m_stage == Stage::ToRoll && (!IsHuman(m_toPlay) || m_autoRoll)) || (m_stage == Stage::Moving && m_doneClock > 0) ||
                         (m_stage == Stage::Over && m_mode == Demo);
    // The places a checker may go pulse, and so does the hint.
    const bool pulsing = HumanToMove() && (m_selected >= 0 || m_hint.has_value());
    // The banner fades to its target, which Update reaches exactly, and counts its time down.
    return waiting || pulsing || m_hintWanted || m_camera.Moving() || m_view.Animating() || !m_toasts.empty() || m_bannerTime > 0 ||
           m_bannerFade != BannerTarget() || std::abs(m_panelSlide - (m_panelVisible ? 1.0f : 0.0f)) > 0.001f;
}

float App::BannerTarget() const
{
    return m_bannerTime != 0 ? 1.0f : 0.0f;
}

render::Scene App::BuildScene() const
{
    render::Scene scene;
    const float aspect = static_cast<float>(m_width) / std::max(1.0f, static_cast<float>(m_height));
    scene.view = m_camera.View();
    scene.projection = m_camera.Projection(aspect);
    scene.eye = m_camera.Eye();
    scene.time = m_time;

    auto mark = [&](int place, int component, float value) {
        if (place >= 0 && place < nardy::Points) {
            Vec4& p = scene.points[static_cast<std::size_t>(place)];
            float* values[4] = {&p.x, &p.y, &p.z, &p.w};
            *values[component] = std::max(*values[component], value);
        }
    };
    // The last turn's steps, until the next player moves.
    if (m_played.empty() || m_stage == Stage::Over)
        for (const int place : m_lastMove)
            mark(place, 2, 1);

    std::array<float, BoardView::Places> glow{};
    int glowSide = -1;
    if (HumanToMove() && m_doneClock <= 0) {
        glowSide = m_toPlay;
        for (const Step& step : NextSteps()) {
            const int place = ToPlace(step.from);
            glow[static_cast<std::size_t>(place)] = std::max(glow[static_cast<std::size_t>(place)], m_selected < 0 ? 0.32f : 0.16f);
            if (m_selected < 0)
                mark(place, 1, 1);
        }
        if (m_hoverPlace >= 0 && glow[static_cast<std::size_t>(m_hoverPlace)] > 0)
            glow[static_cast<std::size_t>(m_hoverPlace)] = 0.7f;
        if (m_selected >= 0) {
            for (const Route& route : RoutesFrom(FromPlace(m_selected))) {
                const int place = ToPlace(route.to);
                const float strength = place == m_hoverPlace ? 1.6f : 1.0f;
                if (place == BoardView::OffPlace)
                    (m_toPlay == 0 ? scene.trays.x : scene.trays.y) = strength;
                else
                    mark(place, 0, strength);
            }
        }
    }
    if (m_hint && HumanToMove()) {
        for (int i = 0; i < m_hint->count; ++i) {
            const Step& step = m_hint->steps[static_cast<std::size_t>(i)];
            mark(ToPlace(step.from), 3, 1);
            if (step.to == nardy::BearOff)
                scene.trays.z = m_toPlay == 0 ? 1.0f : -1.0f;
            else
                mark(ToPlace(step.to), 3, 1);
        }
    }

    std::vector<BoardView::Piece> pieces;
    m_view.AddPieces(pieces, glow, glowSide);
    scene.pieces.reserve(pieces.size());
    for (const BoardView::Piece& piece : pieces)
        scene.pieces.push_back({piece.model, piece.kind, piece.side, piece.value, piece.highlight, piece.dim});
    scene.panels = FrostedPanels();
    return scene;
}

// The game -----------------------------------------------------------------------------------------

bool App::IsHuman(int side) const
{
    switch (m_mode) {
    case VsComputer:
        return side == 0;
    case TwoPlayers:
        return true;
    default:
        return false;
    }
}

nardy::Position App::MoverView() const
{
    return m_toPlay == 0 ? m_board : nardy::Flip(m_board, GameVariant());
}

void App::SetFromMover(const nardy::Position& mover)
{
    m_board = m_toPlay == 0 ? mover : nardy::Flip(mover, GameVariant());
}

int App::ToPlace(int point) const
{
    if (point == nardy::FromBar)
        return BoardView::BarPlace;
    if (point == nardy::BearOff)
        return BoardView::OffPlace;
    if (m_toPlay == 0)
        return point;
    // Black counts the points from the other end: short nardy the opposite way round, long nardy from the far side.
    return GameVariant() == nardy::Variant::Short ? nardy::Points - 1 - point : (point + 12) % nardy::Points;
}

int App::FromPlace(int place) const
{
    if (place == BoardView::BarPlace)
        return nardy::FromBar;
    if (place == BoardView::OffPlace)
        return nardy::BearOff;
    if (place < 0)
        return -100;
    return ToPlace(place); // the same reckoning, both ways
}

// A game in progress as text: the game, who is to play, the first moves still to make, the dice if thrown, and the
// board as White sees it when the turn began.
std::string App::SaveGame() const
{
    if (m_stage == Stage::Opening || m_stage == Stage::Over)
        return {};
    const nardy::Position& board = m_rolled ? m_turnStart : m_board;
    std::string text = std::format("{} {} {} {} {} {} {}", m_game, m_toPlay, m_firstMove[0] ? 1 : 0, m_firstMove[1] ? 1 : 0, m_rolled ? 1 : 0, m_roll.first,
                                   m_roll.second);
    for (const std::int8_t n : board.points)
        text += std::format(" {}", n);
    text += std::format(" {} {} {} {}", board.bar[0], board.bar[1], board.off[0], board.off[1]);
    return text;
}

bool App::LoadGame(const std::string& text)
{
    std::vector<int> values;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (p < end) {
        while (p < end && *p == ' ')
            ++p;
        if (p == end)
            break;
        int value = 0;
        const auto [next, error] = std::from_chars(p, end, value);
        if (error != std::errc())
            return false;
        values.push_back(value);
        p = next;
    }
    if (values.size() != 35 || values[0] != m_game || values[1] < 0 || values[1] > 1 || values[5] < 1 || values[5] > 6 || values[6] < 1 || values[6] > 6)
        return false;
    nardy::Position board;
    std::array<int, 2> count{};
    for (std::size_t i = 0; i < nardy::Points; ++i) {
        const int n = values[7 + i];
        if (n < -nardy::Checkers || n > nardy::Checkers)
            return false;
        board.points[i] = static_cast<std::int8_t>(n);
        count[n > 0 ? 0 : 1] += std::abs(n);
    }
    for (std::size_t side = 0; side < 2; ++side) {
        const int bar = values[31 + side], off = values[33 + side];
        if (bar < 0 || off < 0 || off >= nardy::Checkers || (bar > 0 && GameVariant() == nardy::Variant::Long))
            return false;
        board.bar[side] = static_cast<std::int8_t>(bar);
        board.off[side] = static_cast<std::int8_t>(off);
        count[side] += bar + off;
    }
    if (count[0] != nardy::Checkers || count[1] != nardy::Checkers)
        return false;

    m_board = board;
    m_toPlay = values[1];
    m_firstMove = {values[2] != 0, values[3] != 0};
    m_roll = {values[5], values[6]};
    m_winner = -1;
    m_view.Reset(m_board);
    m_view.ClearDice();
    m_turn = 1;
    if (values[4] != 0) {
        m_rolled = true;
        m_turnStart = m_board;
        m_view.ShowDice(m_toPlay, m_roll.first, m_roll.second);
        BeginMoves();
    }
    else {
        StartTurn();
    }
    return true;
}

void App::NewGame()
{
    StopComputer();
    m_board = nardy::Start(GameVariant());
    m_firstMove = {true, true};
    m_rolled = false;
    m_played.clear();
    m_sequences.clear();
    m_lastMove.clear();
    m_winner = -1;
    m_winPoints = 0;
    m_turn = 0;
    m_doneClock = -1;
    m_undo.clear();
    m_history.clear();
    m_historyScroll = 0;
    m_followHistory = true;
    m_bannerTime = 0;
    Select(-1);
    m_view.Reset(m_board);
    m_view.ClearDice();
    m_sound.Play(SoundBank::Effect::Setup);
    ThrowOpening();
    Save();
}

void App::ThrowOpening()
{
    std::uniform_int_distribution<int> die(1, 6);
    m_opening = {die(m_random), die(m_random)};
    m_view.ThrowOpening(m_opening[0], m_opening[1], m_random);
    m_sound.Play(SoundBank::Effect::Rattle);
    m_stage = Stage::Opening;
    m_stageClock = 0;
}

// The dice of the opening have settled: the higher starts, or both throw again.
void App::FinishOpening()
{
    if (m_opening[0] == m_opening[1]) {
        ShowToast(std::vformat(T(Str::OpeningTie), std::make_format_args(m_opening[0], m_opening[1])));
        ThrowOpening();
        return;
    }
    m_toPlay = m_opening[0] > m_opening[1] ? 0 : 1;
    const std::string_view starts = Pick(m_toPlay, Str::StartsYou, Str::StartsComputer, Str::StartsWhite, Str::StartsBlack);
    ShowToast(std::string(starts));
    AddHistory(std::format("{} ({}:{})", starts, m_opening[0], m_opening[1]), 2);
    if (GameVariant() == nardy::Variant::Short) {
        // In short nardy the opening dice are the first roll.
        ++m_turn;
        m_roll = {m_opening[0], m_opening[1]};
        m_rolled = true;
        m_turnStart = m_board;
        BeginMoves();
    }
    else {
        StartTurn();
    }
    Save();
}

void App::StartTurn()
{
    ++m_turn;
    m_stage = Stage::ToRoll;
    m_stageClock = 0;
    m_rolled = false;
    m_played.clear();
    m_sequences.clear();
    m_doneClock = -1;
    m_hint.reset();
    m_hintWanted = false;
    Select(-1);
}

void App::RollDice()
{
    if (m_stage != Stage::ToRoll)
        return;
    std::uniform_int_distribution<int> die(1, 6);
    m_roll = {die(m_random), die(m_random)};
    m_rolled = true;
    m_turnStart = m_board;
    m_view.Throw(m_toPlay, m_roll.first, m_roll.second, m_random);
    m_sound.Play(SoundBank::Effect::Rattle);
    m_stage = Stage::Rolling;
    m_stageClock = 0;
}

// The dice lie still: every way to play them is found, and the player to move takes over.
void App::BeginMoves()
{
    m_turnStart = m_board;
    m_played.clear();
    m_sequences = nardy::LegalSequences(MoverView(), GameVariant(), m_roll, m_firstMove[static_cast<std::size_t>(m_toPlay)]);
    m_doneClock = -1;
    m_hint.reset();
    m_hintWanted = false;
    m_hintWorker.Cancel();
    Select(-1);
    m_view.UseDice(0, m_roll.Double(), 0);
    m_stageClock = 0;
    if (m_sequences.empty()) {
        // Nothing can be played: the turn passes.
        m_stage = Stage::Moving;
        m_doneClock = PassPause;
        ShowToast(std::string(T(Str::NoMovesToast)));
        m_sound.Play(SoundBank::Effect::NoMove);
        return;
    }
    if (IsHuman(m_toPlay)) {
        if (!m_undo.empty() && m_undo.back().turn >= m_turn)
            m_undo.pop_back();
        m_undo.push_back({m_board, m_toPlay, m_firstMove, m_roll, m_history.size(), m_turn});
        if (m_undo.size() > 300)
            m_undo.erase(m_undo.begin());
        m_stage = Stage::Moving;
        return;
    }
    m_stage = Stage::Thinking;
    m_computerMove.reset();
    const nardy::Network* net = &Net();
    const nardy::Position position = MoverView();
    const nardy::Variant variant = GameVariant();
    const nardy::Roll roll = m_roll;
    const bool first = m_firstMove[static_cast<std::size_t>(m_toPlay)], opponentFirst = m_firstMove[static_cast<std::size_t>(1 - m_toPlay)];
    const int level = m_level;
    const std::uint64_t seed = m_random();
    m_computer.Start([=](std::stop_token stop) { return nardy::Ai::Choose(*net, position, variant, roll, first, opponentFirst, level, seed, stop); });
}

void App::PlayStep(const Step& step)
{
    const nardy::Position after = nardy::Apply(MoverView(), step);
    m_view.Move(m_toPlay, ToPlace(step.from), ToPlace(step.to));
    SetFromMover(after);
    if (m_played.empty())
        m_lastMove.clear();
    m_played.push_back(step);
    m_view.UseDice(static_cast<int>(m_played.size()), m_roll.Double(), m_played.front().die == m_roll.first ? 0 : 1);
    m_hint.reset();
    m_hintWanted = false;
    m_hintWorker.Cancel();
    Select(-1);
    if (IsHuman(m_toPlay) && NextSteps().empty())
        m_doneClock = TurnPause;
}

void App::TakeBackStep()
{
    if (!HumanToMove() || m_played.empty())
        return;
    const Step step = m_played.back();
    m_played.pop_back();
    nardy::Position mover = m_toPlay == 0 ? m_turnStart : nardy::Flip(m_turnStart, GameVariant());
    for (const Step& played : m_played)
        mover = nardy::Apply(mover, played);
    SetFromMover(mover);
    // The checker flies back, and one it knocked off returns from the bar.
    m_view.Move(m_toPlay, ToPlace(step.to), ToPlace(step.from));
    if (step.hit)
        m_view.Move(1 - m_toPlay, BoardView::BarPlace, ToPlace(step.to));
    m_view.UseDice(static_cast<int>(m_played.size()), m_roll.Double(), m_played.empty() || m_played.front().die == m_roll.first ? 0 : 1);
    m_doneClock = -1;
    Select(-1);
}

void App::TakeBackTurn()
{
    while (HumanToMove() && !m_played.empty())
        TakeBackStep();
}

// Takes back this turn's steps, or else goes back to the start of the last turn a person played, with its dice.
void App::Undo()
{
    if (m_mode == Demo)
        return;
    if (HumanToMove() && !m_played.empty()) {
        TakeBackTurn();
        return;
    }
    while (!m_undo.empty() && m_undo.back().turn >= m_turn && m_stage != Stage::Over)
        m_undo.pop_back();
    if (m_undo.empty()) {
        ShowToast(std::string(T(Str::NothingToUndo)));
        return;
    }
    const Snapshot snapshot = m_undo.back();
    StopComputer();
    if (m_stage == Stage::Over && m_winner >= 0) {
        m_score[static_cast<std::size_t>(m_winner)] -= m_winPoints;
        m_bannerTime = 0;
    }
    m_winner = -1;
    m_board = snapshot.board;
    m_toPlay = snapshot.toPlay;
    m_firstMove = snapshot.firstMove;
    m_roll = snapshot.roll;
    m_turn = snapshot.turn;
    m_history.resize(std::min(m_history.size(), snapshot.history));
    m_lastMove.clear();
    m_rolled = true;
    m_view.Reset(m_board);
    m_view.ShowDice(m_toPlay, m_roll.first, m_roll.second);
    BeginMoves();
    Save();
}

void App::EndTurn()
{
    m_doneClock = -1;
    const int side = m_toPlay;
    const int high = std::max(m_roll.first, m_roll.second), low = std::min(m_roll.first, m_roll.second);
    const std::string moves = m_played.empty() ? std::string(T(Str::NoMoves)) : MoveText(m_played);
    AddHistory(std::format("{} {}-{}: {}", Name(side), high, low, moves), side);
    // Where each checker moved from and to: a checker going on with the next step marks only its start and its end.
    m_lastMove.clear();
    for (std::size_t i = 0; i < m_played.size(); ++i) {
        if (i == 0 || m_played[i].from != m_played[i - 1].to)
            m_lastMove.push_back(ToPlace(m_played[i].from));
        if (i + 1 == m_played.size() || m_played[i + 1].from != m_played[i].to)
            m_lastMove.push_back(ToPlace(m_played[i].to));
    }
    m_firstMove[static_cast<std::size_t>(side)] = false;
    if (m_board.off[static_cast<std::size_t>(side)] == nardy::Checkers) {
        FinishGame();
        return;
    }
    m_toPlay = 1 - side;
    StartTurn();
    Save();
}

void App::FinishGame()
{
    const int side = m_toPlay;
    m_winner = side;
    m_winPoints = nardy::WinPoints(MoverView(), GameVariant());
    m_score[static_cast<std::size_t>(side)] += m_winPoints;
    m_stage = Stage::Over;
    m_stageClock = 0;
    const std::string_view title = Pick(side, Str::WinYou, Str::WinComputer, Str::WinWhite, Str::WinBlack);
    const std::string_view points = T(m_winPoints >= 3 ? Str::PointsKoks : m_winPoints == 2 ? Str::PointsMars : Str::PointsOne);
    const std::string score = std::vformat(T(Str::Score), std::make_format_args(m_score[0], m_score[1]));
    ShowBanner(std::string(title), std::format("{}  ·  {}", points, score), -1);
    AddHistory(std::format("{} — {}", title, points), 3);
    m_sound.Play(SoundBank::Effect::Win);
    Save();
}

void App::StopComputer()
{
    m_computer.Cancel();
    m_computerMove.reset();
    m_hintWorker.Cancel();
    m_hintWanted = false;
}

void App::UpdateComputer(float seconds)
{
    if (m_stage == Stage::Thinking) {
        if (!m_computerMove)
            m_computerMove = m_computer.Take();
        if (m_computerMove && m_stageClock >= MinThink) {
            m_stage = Stage::Playing;
            m_stageClock = 0;
            m_computerStep = 0;
            m_stepClock = 0.1f;
        }
        return;
    }
    m_stepClock -= seconds;
    if (m_stepClock > 0 || !m_computerMove)
        return;
    if (m_computerStep < m_computerMove->count) {
        PlayStep(m_computerMove->steps[static_cast<std::size_t>(m_computerStep++)]);
        m_stepClock = m_computerStep < m_computerMove->count ? StepPause : TurnPause + 0.2f;
    }
    else {
        EndTurn();
    }
}

// The computer's advice for the whole roll, at its best: any steps already played are taken back first.
void App::RequestHint()
{
    if (!HumanToMove() || m_sequences.empty() || m_doneClock > 0)
        return;
    if (m_hint) {
        // Asked again: the move is played.
        const nardy::Move move = *m_hint;
        for (int i = 0; i < move.count; ++i)
            PlayStep(move.steps[static_cast<std::size_t>(i)]);
        return;
    }
    TakeBackTurn();
    m_hintWanted = true;
    const nardy::Network* net = &Net();
    const nardy::Position position = MoverView();
    const nardy::Variant variant = GameVariant();
    const nardy::Roll roll = m_roll;
    const bool first = m_firstMove[static_cast<std::size_t>(m_toPlay)], opponentFirst = m_firstMove[static_cast<std::size_t>(1 - m_toPlay)];
    const std::uint64_t seed = m_random();
    m_hintWorker.Start([=](std::stop_token stop) { return nardy::Ai::Choose(*net, position, variant, roll, first, opponentFirst, 5, seed, stop); });
}

void App::UpdateHint()
{
    if (!m_hintWanted)
        return;
    std::optional<nardy::Move> move = m_hintWorker.Take();
    if (!move)
        return;
    m_hintWanted = false;
    if (!HumanToMove() || !m_played.empty())
        return;
    m_hint = move;
    const std::vector<Step> steps(move->steps.begin(), move->steps.begin() + move->count);
    const std::string text = MoveText(steps);
    ShowToast(std::vformat(T(Str::HintText), std::make_format_args(text)));
}

std::vector<Step> App::NextSteps() const
{
    std::vector<Step> steps;
    const std::size_t k = m_played.size();
    for (const nardy::Move& move : m_sequences) {
        if (move.count <= static_cast<int>(k) || !StartsWith(move, m_played))
            continue;
        const Step& next = move.steps[k];
        if (std::ranges::find(steps, next) == steps.end())
            steps.push_back(next);
    }
    return steps;
}

// Where the checker on `point` may go this turn, by one step or by several of its own; a shorter way is preferred,
// and among equals one that hits on the way.
std::vector<App::Route> App::RoutesFrom(int point) const
{
    std::vector<Route> routes;
    const std::size_t k = m_played.size();
    auto hits = [](const std::vector<Step>& steps) { return std::ranges::count_if(steps, [](const Step& s) { return s.hit; }); };
    for (const nardy::Move& move : m_sequences) {
        if (move.count <= static_cast<int>(k) || move.steps[k].from != point || !StartsWith(move, m_played))
            continue;
        std::vector<Step> chain;
        for (std::size_t j = k; j < static_cast<std::size_t>(move.count); ++j) {
            const Step& step = move.steps[j];
            if (!chain.empty() && (step.from != chain.back().to || chain.back().to == nardy::BearOff))
                break;
            chain.push_back(step);
            const auto found = std::ranges::find_if(routes, [&](const Route& route) { return route.to == step.to; });
            if (found == routes.end())
                routes.push_back({step.to, chain});
            else if (chain.size() < found->steps.size() || (chain.size() == found->steps.size() && hits(chain) > hits(found->steps)))
                found->steps = chain;
        }
    }
    return routes;
}

void App::PlayRoute(const Route& route)
{
    for (const Step& step : route.steps)
        PlayStep(step);
}

// The checker on `point` goes by the higher die it may use, else the lower.
bool App::PlayLargerDie(int point)
{
    std::optional<Step> best;
    for (const Step& step : NextSteps())
        if (step.from == point && (!best || step.die > best->die))
            best = step;
    if (!best)
        return false;
    PlayStep(*best);
    return true;
}

void App::Select(int place)
{
    m_selected = place;
    if (place >= 0)
        m_view.Lift(m_toPlay, place);
    else
        m_view.Lift(-1, -1);
}

void App::DropSelection()
{
    if (m_gesture == Gesture::Drag) {
        m_gesture = Gesture::None;
        m_view.Move(m_toPlay, m_selected, m_selected);
    }
    Select(-1);
}

// Why a checker of the player to move may not go now, in words.
std::string App::WhyNot(int point) const
{
    const nardy::Position mover = MoverView();
    if (GameVariant() == nardy::Variant::Short && mover.bar[0] > 0 && point != nardy::FromBar)
        return std::string(T(Str::EnterFirst));
    if (GameVariant() == nardy::Variant::Long && point == nardy::Head && std::ranges::any_of(m_played, [](const Step& s) { return s.from == nardy::Head; }))
        return std::string(T(Str::HeadRule));
    return std::string(T(Str::CannotMove));
}

void App::SetGame(int game)
{
    m_game = (game + 2) % 2;
    m_score = {};
    NewGame();
}

void App::SetMode(int mode)
{
    mode = (mode + 3) % 3;
    if (mode == m_mode)
        return;
    const bool wasHuman = IsHuman(m_toPlay);
    m_mode = mode;
    m_score = {};
    // The player to move may have changed hands: their turn starts again for the new player.
    if (wasHuman != IsHuman(m_toPlay) && (m_stage == Stage::Moving || m_stage == Stage::Thinking || m_stage == Stage::Playing) && m_doneClock <= 0) {
        StopComputer();
        m_board = m_turnStart;
        m_view.Reset(m_board);
        BeginMoves();
    }
    Save();
}

void App::SetLevel(int level)
{
    m_level = std::clamp(level, 1, 5);
    Save();
}

void App::SetTheme(int theme)
{
    m_theme = (theme + ThemeCount) % ThemeCount;
    m_renderer->SetTheme(Themes()[static_cast<std::size_t>(m_theme)]);
    Save();
}

void App::SetCamera(int camera)
{
    m_camera.SetMode(static_cast<CameraMode>((camera + CameraModeCount) % CameraModeCount));
    Save();
}

void App::SetAutoRoll(bool on)
{
    m_autoRoll = on;
    m_stageClock = 0;
    Save();
}

void App::Save()
{
    m_settings.theme = m_theme;
    m_settings.level = m_level;
    m_settings.mode = m_mode;
    m_settings.game = m_game;
    m_settings.camera = static_cast<int>(m_camera.Mode());
    m_settings.autoRoll = m_autoRoll;
    m_settings.score = m_score;
    m_settings.sound = m_sound.Enabled();
    m_settings.panel = m_panelVisible;
    m_settings.language = m_language;
    m_settings.match = SaveGame();
    m_settings.history.clear();
    if (!m_settings.match.empty()) {
        const std::size_t first = m_history.size() > 80 ? m_history.size() - 80 : 0;
        for (std::size_t i = first; i < m_history.size(); ++i)
            m_settings.history += std::format("{}{}{}", i > first ? "|" : "", m_history[i].kind, m_history[i].text);
    }
    if (m_window) {
        const std::uint32_t flags = SDL_GetWindowFlags(m_window.get());
        if (!m_fullScreen && (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0)
            SDL_GetWindowSize(m_window.get(), &m_normalSize.width, &m_normalSize.height);
        m_settings.windowWidth = m_normalSize.width;
        m_settings.windowHeight = m_normalSize.height;
        m_settings.maximized = m_fullScreen ? m_maximizedBeforeFullScreen : (flags & SDL_WINDOW_MAXIMIZED) != 0;
    }
    m_settings.Save();
}

void App::ToggleSound()
{
    m_sound.SetEnabled(!m_sound.Enabled());
    Save();
}

void App::ToggleLanguage()
{
    m_language = m_language == Language::English ? Language::Russian : Language::English;
    SDL_SetWindowTitle(m_window.get(), std::string(T(Str::Title)).c_str());
    Save();
}

void App::ToggleFullScreen()
{
    const bool fullScreen = !m_fullScreen;
    if (fullScreen)
        m_maximizedBeforeFullScreen = (SDL_GetWindowFlags(m_window.get()) & SDL_WINDOW_MAXIMIZED) != 0;
    // Borderless over the whole screen, at its own resolution.
    if (SDL_SetWindowFullscreen(m_window.get(), fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
        m_fullScreen = fullScreen;
}

void App::OpenLink(const char* url)
{
    if (SDL_OpenURL(url) == 0)
        return;
    // Where no browser could be started, the link goes to the clipboard instead.
    SDL_SetClipboardText(url);
    ShowToast(std::string(T(Str::LinkCopied)));
}

void App::AddHistory(std::string text, int kind)
{
    m_history.push_back({std::move(text), kind});
    m_followHistory = true;
}

// A turn as players write it, from the mover's side: one checker's steps joined ("24/13", or "24/18*/13" when it
// hit on the way), and the same move made twice counted ("13/7(2)").
std::string App::MoveText(const std::vector<Step>& steps) const
{
    auto name = [&](int point) {
        if (point == nardy::FromBar)
            return std::string(T(Str::Bar));
        if (point == nardy::BearOff)
            return std::string(T(Str::Off));
        return std::to_string(point + 1);
    };
    std::vector<std::string> parts;
    for (std::size_t i = 0; i < steps.size();) {
        std::size_t j = i;
        while (j + 1 < steps.size() && steps[j + 1].from == steps[j].to && steps[j].to != nardy::BearOff)
            ++j;
        std::string part = name(steps[i].from);
        for (std::size_t k = i; k <= j; ++k)
            if (k == j || steps[k].hit)
                part += "/" + name(steps[k].to) + (steps[k].hit ? "*" : "");
        parts.push_back(std::move(part));
        i = j + 1;
    }
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (std::find(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(i), parts[i]) != parts.begin() + static_cast<std::ptrdiff_t>(i))
            continue;
        const auto times = std::count(parts.begin(), parts.end(), parts[i]);
        text += (text.empty() ? "" : " ") + parts[i] + (times > 1 ? std::format("({})", times) : "");
    }
    return text;
}

// Input --------------------------------------------------------------------------------------------

bool App::PointerRay(Vec3& origin, Vec3& direction) const
{
    if (m_pointer.x < 0 || m_pointer.y < 0)
        return false;
    m_camera.Ray(m_pointer.x, m_pointer.y, static_cast<float>(m_width), static_cast<float>(m_height), origin, direction);
    return true;
}

std::optional<Vec3> App::PointerOnPlane(float height) const
{
    Vec3 origin, direction;
    if (!PointerRay(origin, direction) || direction.y > -1e-4f)
        return std::nullopt;
    const float t = (height - origin.y) / direction.y;
    if (t < 0)
        return std::nullopt;
    return origin + direction * t;
}

int App::PlaceUnderPointer() const
{
    const std::optional<Vec3> hit = PointerOnPlane(board::CheckerHeight);
    if (!hit)
        return -1;
    const Vec2 xz{hit->x, hit->z};
    const int point = board::PointAt(xz);
    if (point >= 0)
        return point;
    if (std::abs(xz.x) < board::BarHalf && std::abs(xz.y) < board::HalfDepth)
        return BoardView::BarPlace;
    const float trayLeft = board::FieldRight + board::RimWidth * 0.5f;
    if (xz.x > trayLeft && xz.x < trayLeft + board::RimWidth + board::TrayWidth && std::abs(xz.y) < board::HalfDepth + board::RimWidth)
        return BoardView::OffPlace;
    return -1;
}

void App::HandleBoardPointer()
{
    const bool free = !m_pointer.consumed && !DialogOpen();
    m_hoverPlace = free && m_gesture != Gesture::TurnCamera ? PlaceUnderPointer() : -1;
    const bool moving = HumanToMove() && m_doneClock <= 0;
    const std::vector<Step> next = moving ? NextSteps() : std::vector<Step>{};
    auto movable = [&](int place) {
        const int point = FromPlace(place);
        return std::ranges::any_of(next, [&](const Step& step) { return step.from == point; });
    };
    std::vector<Route> routes;
    if (moving && m_selected >= 0)
        routes = RoutesFrom(FromPlace(m_selected));
    auto routeTo = [&](int place) {
        return std::ranges::find_if(routes, [&](const Route& route) { return ToPlace(route.to) == place; });
    };
    const bool awaitingRoll = m_stage == Stage::ToRoll && IsHuman(m_toPlay) && !m_autoRoll;
    if (m_hoverPlace >= 0 && (movable(m_hoverPlace) || routeTo(m_hoverPlace) != routes.end()))
        m_cursorHand = true;
    if (free && awaitingRoll && m_hoverPlace >= 0)
        m_cursorHand = true;

    if (free && m_gesture == Gesture::None && (m_pointer.leftPressed || m_pointer.rightPressed)) {
        m_gesture = Gesture::Press;
        m_pressRight = !m_pointer.leftPressed;
        m_pressX = m_dragX = m_pointer.x;
        m_pressY = m_dragY = m_pointer.y;
        m_pressPlace = m_hoverPlace;
        m_pressDouble = m_pointer.doubleClick;
    }

    if (m_gesture == Gesture::Press) {
        const bool moved = std::hypot(m_pointer.x - m_pressX, m_pointer.y - m_pressY) > DragStart * Scale();
        const bool down = m_pressRight ? m_pointer.rightDown : m_pointer.leftDown;
        if (moved && down) {
            if (!m_pressRight && moving && movable(m_pressPlace)) {
                m_gesture = Gesture::Drag;
                Select(m_pressPlace);
            }
            else {
                m_gesture = Gesture::TurnCamera;
            }
        }
        else if (!down) {
            // A click.
            m_gesture = Gesture::None;
            const int place = m_pressPlace;
            if (m_pressRight) {
                if (HumanToMove() && !m_played.empty())
                    TakeBackStep();
                else
                    Select(-1);
            }
            else if (awaitingRoll) {
                if (place >= 0)
                    RollDice();
            }
            else if (moving) {
                const auto route = routeTo(place);
                const int toggled = std::exchange(m_toggledPlace, -1);
                if (m_pressDouble && toggled == place && movable(place)) {
                    PlayLargerDie(FromPlace(place));
                }
                else if (m_selected >= 0 && route != routes.end()) {
                    PlayRoute(*route);
                }
                else if (movable(place)) {
                    Select(place == m_selected ? -1 : place);
                    m_toggledPlace = place;
                }
                else {
                    const nardy::Position mover = MoverView();
                    const int point = FromPlace(place);
                    const bool own = point == nardy::FromBar ? mover.bar[0] > 0 : point >= 0 && point < nardy::Points && mover.points[static_cast<std::size_t>(point)] > 0;
                    if (own)
                        ShowToast(WhyNot(point));
                    Select(-1);
                }
            }
        }
    }

    if (m_gesture == Gesture::TurnCamera) {
        if (m_pressRight ? m_pointer.rightDown : m_pointer.leftDown) {
            m_camera.Rotate(-(m_pointer.x - m_dragX) * 0.006f, (m_pointer.y - m_dragY) * 0.005f);
            m_dragX = m_pointer.x;
            m_dragY = m_pointer.y;
        }
        else {
            m_gesture = Gesture::None;
        }
    }

    if (m_gesture == Gesture::Drag) {
        if (!moving || m_selected < 0) {
            m_gesture = Gesture::None;
            m_view.Carry(std::nullopt);
        }
        else if (m_pointer.leftDown) {
            if (const std::optional<Vec3> at = PointerOnPlane(0.03f))
                m_view.Carry(*at);
        }
        else {
            // Dropped: where it may go, else back to its place.
            m_gesture = Gesture::None;
            const auto route = routeTo(PlaceUnderPointer());
            if (route != routes.end()) {
                PlayRoute(*route);
            }
            else {
                m_view.Move(m_toPlay, m_selected, m_selected);
                Select(-1);
            }
        }
    }

    if (free && m_pointer.wheel != 0)
        m_camera.Zoom(m_pointer.wheel);
}

void App::HandleKey(const SDL_KeyboardEvent& event)
{
    const SDL_Keysym& key = event.keysym;
    const bool control = (key.mod & KMOD_CTRL) != 0;
    const bool shift = (key.mod & KMOD_SHIFT) != 0;
    const bool alt = (key.mod & KMOD_ALT) != 0;
    const bool repeat = event.repeat != 0;
    const char letter = ShortcutLetter(key);

    if (alt) {
        if ((key.sym == SDLK_RETURN || key.sym == SDLK_KP_ENTER) && !repeat)
            ToggleFullScreen();
        return; // the rest of Alt is the desktop's
    }
    if (DialogOpen()) {
        // Over a card only its own keys work, and full screen.
        if (repeat || control)
            return;
        if (key.sym == SDLK_ESCAPE)
            m_dialog = Dialog::None;
        else if (key.sym == SDLK_F1)
            ToggleDialog(Dialog::Help);
        else if (letter == 'i')
            ToggleDialog(Dialog::About);
        else if (key.sym == SDLK_F11)
            ToggleFullScreen();
        return;
    }
    if (control) {
        if (letter == 'z')
            Undo();
        else if (letter == 'n' && !repeat)
            NewGame();
        return;
    }
    // Taking back goes on step by step while the key is held; the other keys act once.
    if (key.sym == SDLK_BACKSPACE) {
        if (HumanToMove() && !m_played.empty())
            TakeBackStep();
        else if (!repeat)
            Undo();
        return;
    }
    if (repeat)
        return;

    switch (key.sym) {
    case SDLK_SPACE:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (m_stage == Stage::ToRoll && IsHuman(m_toPlay))
            RollDice();
        else if (HumanToMove() && m_doneClock > 0)
            EndTurn();
        return;
    case SDLK_HOME: m_camera.Reset(); return;
    case SDLK_TAB:
        m_panelVisible = !m_panelVisible;
        Save();
        return;
    case SDLK_F1: ToggleDialog(Dialog::Help); return;
    case SDLK_F11: ToggleFullScreen(); return;
    case SDLK_ESCAPE:
        if (m_gesture == Gesture::Drag || m_selected >= 0)
            DropSelection();
        else if (m_fullScreen)
            ToggleFullScreen();
        return;
    default: break;
    }
    switch (letter) {
    case 'h': RequestHint(); return;
    case 'a': SetAutoRoll(!m_autoRoll); return;
    case 'c': SetCamera(static_cast<int>(m_camera.Mode()) + 1); return;
    case 'n': NewGame(); return;
    case 'g': SetGame(m_game + 1); return;
    case 'm': SetMode(m_mode + (shift ? -1 : 1)); return;
    case 't': SetTheme(m_theme + (shift ? -1 : 1)); return;
    case 'v': ToggleSound(); return;
    case 'l': ToggleLanguage(); return;
    case 'i': ToggleDialog(Dialog::About); return;
    default: break;
    }
    if (const int level = LevelKey(key))
        SetLevel(level);
}

// The interface ------------------------------------------------------------------------------------

Vec3 App::Accent() const
{
    return Themes()[static_cast<std::size_t>(m_theme)].accent;
}

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

namespace {

constexpr float HelpWidth = 900, HelpRow = 26, HelpPad = 28, HelpColumnGap = 24;
constexpr float AboutWidth = 520, AboutPad = 28, AboutIcon = 76, LinkRow = 36, LinkGap = 6;

// The about card is in English, whatever the language of the interface.
constexpr std::string_view AboutDescription =
    "A 3D nardy game written in modern C++ on OpenGL\u00A04.6: long nardy and short nardy, or backgammon, against a computer opponent of "
    "five levels, from Novice to Master, that judges positions with neural networks it taught itself. Everything you see and hear is made "
    "in code: there is no model, texture or sound file.";

struct Link {
    std::string_view name;
    std::string_view address; // as shown
    const char* url;
};

constexpr std::array<Link, 5> AuthorLinks{{
    {"GitHub", "github.com/makarov-mm", "https://github.com/makarov-mm"},
    {"GitHub Sponsors", "github.com/sponsors/makarov-mm", "https://github.com/sponsors/makarov-mm"},
    {"LinkedIn", "linkedin.com/in/makarov-mm", "https://www.linkedin.com/in/makarov-mm/"},
    {"Threads", "threads.net/@m.m.makarov", "https://www.threads.net/@m.m.makarov"},
    {"Instagram", "instagram.com/m.m.makarov", "https://www.instagram.com/m.m.makarov/"},
}};

} // namespace

std::vector<std::string> App::WrapText(FontId font, std::string_view words, float width) const
{
    std::vector<std::string> lines;
    std::string line;
    while (!words.empty()) {
        const std::size_t space = std::min(words.find(' '), words.size());
        const std::string_view word = words.substr(0, space);
        words.remove_prefix(std::min(space + 1, words.size()));
        std::string longer = line.empty() ? std::string(word) : std::format("{} {}", line, word);
        if (!line.empty() && m_painter->Measure(font, longer) > width) {
            lines.push_back(std::move(line));
            line = word;
        }
        else {
            line = std::move(longer);
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

std::array<App::HelpLine, 2 * App::HelpRows> App::HelpLines() const
{
    return {{
        // The mouse and the turn.
        {T(Str::KeyClick), Str::HelpClick},
        {T(Str::KeyDrag), Str::HelpDrag},
        {T(Str::KeyDoubleClick), Str::HelpDoubleClick},
        {T(Str::KeyRightClick), Str::HelpTakeBack},
        {T(Str::KeyRightDrag), Str::HelpRotate},
        {T(Str::KeyWheel), Str::HelpZoom},
        {"Space  Enter", Str::HelpRoll},
        {"H", Str::HelpHint},
        {"Ctrl+Z", Str::HelpUndo},
        {"A", Str::HelpAutoRoll},
        {"C  Home", Str::HelpCamera},
        // The game and the window.
        {"N", Str::HelpNew},
        {"G", Str::HelpGame},
        {"M", Str::HelpMode},
        {"1 – 5", Str::HelpLevel},
        {"T", Str::HelpTheme},
        {"V", Str::HelpSound},
        {"L", Str::HelpLanguage},
        {"Tab", Str::HelpPanel},
        {"F11  Alt+Enter", Str::HelpFullScreen},
        {"I", Str::HelpAbout},
        {"Esc  F1", Str::HelpClose},
    }};
}

App::HelpLayout App::LayOutHelp() const
{
    const float scale = Scale();
    const ui::Painter& p = *m_painter;
    HelpLayout layout;
    // The columns as they were laid out, or wider where the desktop's font needs it.
    layout.keys = {228 * scale, 130 * scale};
    std::array<float, 2> actions{};
    const auto lines = HelpLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t column = i / HelpRows;
        layout.keys[column] = std::max(layout.keys[column], p.Measure(FontId::Body, lines[i].keys) + 16 * scale);
        actions[column] = std::max(actions[column], p.Measure(FontId::Body, T(lines[i].action)));
    }
    const float first = layout.keys[0] + actions[0] + HelpColumnGap * scale, second = layout.keys[1] + actions[1];
    const float inner = std::max((HelpWidth - 2 * HelpPad) * scale, first + second);
    layout.second = std::max(inner * 0.57f, first);
    layout.width = std::max(inner, layout.second + second) + 2 * HelpPad * scale;

    const Str rules = GameVariant() == nardy::Variant::Long ? Str::RulesLong : Str::RulesShort;
    layout.rules = WrapText(FontId::Body, T(rules), layout.width - 2 * HelpPad * scale);
    // The rows close up, down to the height of a line, where the window is too low for them.
    const auto rows = static_cast<float>(HelpRows + 1 + layout.rules.size());
    const float fixed = (2 * HelpPad + 44 + 12) * scale;
    const float room = static_cast<float>(m_height) - 20 * scale - fixed;
    layout.rowHeight = std::max(p.LineHeight(FontId::Body), std::min(room / rows, HelpRow * scale));
    layout.height = fixed + rows * layout.rowHeight;
    return layout;
}

Rect App::CenteredCard(float width, float height) const
{
    const Rect free = FreeArea();
    const float x = width <= free.width ? free.x + (free.width - width) / 2 : std::max(0.0f, (static_cast<float>(m_width) - width) / 2);
    return {x, std::max(10 * Scale(), (static_cast<float>(m_height) - height) / 2), width, height};
}

Rect App::HelpCard() const
{
    const HelpLayout layout = LayOutHelp();
    return CenteredCard(layout.width, layout.height);
}

Rect App::AboutCard() const
{
    const float scale = Scale();
    const ui::Painter& p = *m_painter;
    const auto lines = static_cast<float>(WrapText(FontId::Body, AboutDescription, (AboutWidth - 2 * AboutPad) * scale).size());
    const auto links = static_cast<float>(AuthorLinks.size());
    const float height = (2 * AboutPad + AboutIcon + 20 + 16 + 2 + 20 + links * (LinkRow + LinkGap) - LinkGap) * scale +
                         lines * (p.LineHeight(FontId::Body) + 3 * scale) + p.LineHeight(FontId::Heading) + p.LineHeight(FontId::Body);
    return CenteredCard(AboutWidth * scale, height);
}

Rect App::BannerCard() const
{
    const float scale = Scale();
    const float width = std::max({m_painter->Measure(FontId::Display, m_bannerTitle) + 80 * scale, m_painter->Measure(FontId::Body, m_bannerDetail) + 60 * scale,
                                  320 * scale});
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
    if (m_dialog == Dialog::Help)
        add(HelpCard(), 20, 1);
    else if (m_dialog == Dialog::About)
        add(AboutCard(), 20, 1);
    return panels;
}

void App::ShowToast(std::string text)
{
    if (m_toasts.size() >= 3)
        m_toasts.erase(m_toasts.begin());
    m_toasts.push_back({std::move(text), 0});
}

void App::ShowBanner(std::string title, std::string detail, float seconds)
{
    m_bannerTitle = std::move(title);
    m_bannerDetail = std::move(detail);
    m_bannerTime = seconds;
}

std::string_view App::Pick(int side, Str you, Str computer, Str white, Str black) const
{
    if (m_mode == VsComputer)
        return T(side == 0 ? you : computer);
    return T(side == 0 ? white : black);
}

std::string App::Name(int side) const
{
    return std::string(Pick(side, Str::NameYou, Str::NameComputer, Str::NameWhite, Str::NameBlack));
}

std::string App::StatusTitle() const
{
    if (m_stage == Stage::Opening)
        return std::string(T(Str::Opening));
    if (m_stage == Stage::Over)
        return std::string(Pick(m_winner, Str::WinYou, Str::WinComputer, Str::WinWhite, Str::WinBlack));
    return std::string(Pick(m_toPlay, Str::TurnYou, Str::TurnComputer, Str::TurnWhite, Str::TurnBlack));
}

std::string App::StatusDetail() const
{
    switch (m_stage) {
    case Stage::Opening:
        return std::string(T(Str::OpeningDetail));
    case Stage::Over:
        return std::string(T(m_mode == Demo ? Str::DemoNext : Str::PressNew));
    case Stage::ToRoll:
        return std::string(T(IsHuman(m_toPlay) && !m_autoRoll ? Str::RollPrompt : Str::Rolling));
    case Stage::Rolling:
        return std::string(T(Str::Rolling));
    case Stage::Thinking:
        return std::string(T(Str::Thinking));
    case Stage::Playing:
        return std::string(T(Str::ComputerMoving));
    case Stage::Moving:
        break;
    }
    if (m_sequences.empty())
        return std::string(T(Str::NoMovesToast));
    if (m_doneClock > 0)
        return std::string(T(Str::TurnDone));
    // The dice still to play.
    std::vector<int> dice = DiceOf(m_roll);
    for (const Step& step : m_played)
        if (const auto used = std::ranges::find(dice, step.die); used != dice.end())
            dice.erase(used);
    std::string left;
    for (const int die : dice)
        left += (left.empty() ? "" : ", ") + std::to_string(die);
    return std::vformat(T(Str::ToPlay), std::make_format_args(left));
}

void App::ToggleDialog(Dialog dialog)
{
    m_dialog = m_dialog == dialog ? Dialog::None : dialog;
    m_dialogOpening = m_dialog != Dialog::None;
    if (!m_dialogOpening)
        return;
    // What the mouse was doing stops: a checker carried is not dropped, nor the view turned, behind the card.
    if (m_gesture == Gesture::Drag)
        DropSelection();
    m_gesture = Gesture::None;
    m_activeWidget = 0;
}

bool App::PressedOutside(Rect card) const
{
    // A quick click, such as a tap on a touchpad, presses and lets go within one frame: when it opens a card, its
    // press must not then close it.
    return m_pointer.leftPressed && !m_dialogOpening && !card.Contains(m_pointer.x, m_pointer.y);
}

App::Interaction App::Interact(int id, Rect rect, bool enabled)
{
    // While a card is open, only its own widgets answer, from the frame after the one that opened it; and none while
    // the mouse carries a checker or turns the view.
    const bool interactive = enabled && (!DialogOpen() || (id > ModalWidgets && !m_dialogOpening)) && m_gesture == Gesture::None;
    Interaction result;
    result.hover = interactive && rect.Contains(m_pointer.x, m_pointer.y);
    if (result.hover)
        m_cursorHand = true;
    if (result.hover && m_pointer.leftPressed) {
        m_activeWidget = id;
        m_pointer.consumed = true;
    }
    if (m_pointer.leftReleased && m_activeWidget == id) {
        result.clicked = result.hover;
        m_activeWidget = 0;
        m_pointer.consumed = true;
    }
    result.pressed = result.hover && m_activeWidget == id && m_pointer.leftDown;
    if (result.clicked)
        m_sound.Play(SoundBank::Effect::Click);
    return result;
}

bool App::Button(int id, Rect rect, std::string_view label, bool enabled, bool accent)
{
    const Interaction state = Interact(id, rect, enabled);
    ui::Color fill = !enabled        ? Rgba(255, 255, 255, 7)
                     : state.pressed ? Rgba(255, 255, 255, 52)
                     : state.hover   ? Rgba(255, 255, 255, 34)
                                     : Rgba(255, 255, 255, 17);
    if (accent && enabled)
        fill = ui::Mix(fill, ui::FromLinear(Accent(), static_cast<float>(fill.a) / 255.0f * 2.2f), 0.6f);
    m_painter->Fill(rect, 9 * Scale(), fill);
    m_painter->TextCentered(FontId::Body, rect, m_painter->Elided(FontId::Body, label, rect.width - 8 * Scale()), enabled ? TextColor : ui::Fade(TextColor, 0.35f));
    return state.clicked;
}

int App::Selector(int id, Rect rect, std::string_view label, std::string_view value, float labelWidth)
{
    const float scale = Scale();
    m_painter->Text(FontId::Body, rect.x, rect.y + (rect.height - m_painter->LineHeight(FontId::Body)) / 2, label, MutedColor);
    const float arrow = rect.height;
    const Rect right{rect.Right() - arrow, rect.y, arrow, rect.height};
    // The value past the longest label, from 30% of the row at least: fonts differ in width from one desktop to another.
    const Rect left{rect.x + std::max(rect.width * 0.3f, labelWidth + 12 * scale), rect.y, arrow, rect.height};
    const Rect valueRect{left.Right(), rect.y, right.x - left.Right(), rect.height};
    m_painter->Fill({valueRect.x + 2 * scale, rect.y, valueRect.width - 4 * scale, rect.height}, 9 * scale, Rgba(255, 255, 255, 9));
    m_painter->TextCentered(FontId::Body, valueRect, m_painter->Elided(FontId::Body, value, valueRect.width - 8 * scale), TextColor);
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
    if (m_dialog == Dialog::Help)
        DrawHelp();
    else if (m_dialog == Dialog::About)
        DrawAbout();
}

// A checker seen from above, in its side's colors from the theme.
void App::DrawChecker(float x, float y, float radius, int side)
{
    ui::Painter& p = *m_painter;
    const Theme& theme = Themes()[static_cast<std::size_t>(m_theme)];
    const Vec3 linear = theme.checkers[static_cast<std::size_t>(side)].color;
    // Brightened toward what the lit board shows.
    const ui::Color color = ui::FromLinear(linear * 1.25f + Vec3{0.02f, 0.02f, 0.02f});
    const bool light = linear.x + linear.y + linear.z > 0.6f;
    p.Disc(x, y + radius * 0.14f, radius * 1.06f, Rgba(0, 0, 0, 80));
    p.Disc(x, y, radius, ui::Mix(color, light ? Rgba(0, 0, 0) : Rgba(255, 255, 255), 0.18f));
    p.Disc(x, y, radius * 0.84f, color);
    p.Disc(x, y, radius * 0.5f, ui::Mix(color, light ? Rgba(0, 0, 0) : Rgba(255, 255, 255), 0.1f));
    p.Disc(x, y, radius * 0.42f, color);
}

// A die's face with its pips, darkened once played.
void App::DrawDie(float x, float y, float size, int value, float dim)
{
    ui::Painter& p = *m_painter;
    const Rect face{x - size / 2, y - size / 2, size, size};
    p.Shadow(face, size * 0.15f, Rgba(0, 0, 0, static_cast<int>(70 * (1 - dim * 0.6f))));
    p.Fill(face, size * 0.2f, ui::Mix(Rgba(240, 236, 226), Rgba(90, 92, 100), dim * 0.75f));
    static constexpr std::array<std::array<float, 2>, 9> spots{{{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};
    static constexpr std::array<std::array<int, 6>, 6> layouts{{{4}, {0, 8}, {0, 4, 8}, {0, 2, 6, 8}, {0, 2, 4, 6, 8}, {0, 2, 3, 5, 6, 8}}};
    const ui::Color pip = ui::Mix(Rgba(24, 22, 22), Rgba(60, 62, 70), dim);
    const auto& layout = layouts[static_cast<std::size_t>(std::clamp(value, 1, 6) - 1)];
    for (int i = 0; i < value; ++i) {
        const auto& spot = spots[static_cast<std::size_t>(layout[static_cast<std::size_t>(i)])];
        p.Disc(x + spot[0] * size * 0.27f, y + spot[1] * size * 0.27f, size * 0.095f, pip);
    }
}

void App::DrawStatus(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const ui::Color accent = ui::FromLinear(Accent());
    p.Fill(area, 12 * scale, Rgba(255, 255, 255, 14));

    const int side = m_stage == Stage::Over ? m_winner : m_toPlay;
    if (m_stage != Stage::Opening && side >= 0)
        DrawChecker(area.x + 24 * scale, area.y + area.height / 2, 11 * scale, side);

    // The dice of the turn on the right, the played ones darkened.
    float right = area.Right() - 10 * scale;
    const bool showDice = m_rolled && m_stage != Stage::Rolling && m_stage != Stage::ToRoll && m_stage != Stage::Over;
    if (showDice) {
        const std::vector<int> dice = DiceOf(m_roll);
        const float size = (dice.size() > 2 ? 17 : 22) * scale, gap = 5 * scale;
        std::vector<bool> used(dice.size(), false);
        for (const Step& step : m_played)
            for (std::size_t i = 0; i < dice.size(); ++i)
                if (!used[i] && dice[i] == step.die) {
                    used[i] = true;
                    break;
                }
        for (std::size_t i = dice.size(); i-- > 0;) {
            DrawDie(right - size / 2, area.y + area.height / 2, size, dice[i], used[i] ? 1.0f : 0.0f);
            right -= size + gap;
        }
        right -= 4 * scale;
    }
    if (m_stage == Stage::Thinking) {
        p.Spinner(right - 10 * scale, area.y + area.height / 2, 8 * scale, 2.2f * scale, m_time * 5.0f, accent);
        right -= 26 * scale;
    }

    const float textX = area.x + (m_stage == Stage::Opening ? 16 : 46) * scale, room = right - textX;
    const float lineHeight = p.LineHeight(FontId::Heading);
    p.Text(FontId::Heading, textX, area.y + area.height / 2 - lineHeight + 3 * scale, p.Elided(FontId::Heading, StatusTitle(), room),
           m_stage == Stage::Over ? accent : TextColor);
    p.Text(FontId::Body, textX, area.y + area.height / 2 + 2 * scale, p.Elided(FontId::Body, StatusDetail(), room), MutedColor);
}

// The players: who is to move, their pips and checkers borne off, and the points they have won.
void App::DrawPlayers(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const ui::Color accent = ui::FromLinear(Accent());
    const float rowHeight = 46 * scale;
    for (int side = 0; side < 2; ++side) {
        const float y = area.y + static_cast<float>(side) * rowHeight;
        const bool toMove = m_stage != Stage::Over && m_stage != Stage::Opening && m_toPlay == side;
        const bool winner = m_stage == Stage::Over && m_winner == side;
        if (toMove || winner)
            p.Fill({area.x - 6 * scale, y + 2 * scale, area.width + 12 * scale, rowHeight - 4 * scale}, 9 * scale, ui::Fade(accent, winner ? 0.3f : 0.16f));
        DrawChecker(area.x + 10 * scale, y + rowHeight / 2, 9 * scale, side);
        const std::string score = std::to_string(m_score[static_cast<std::size_t>(side)]);
        const float scoreWidth = p.Measure(FontId::Display, score);
        p.Text(FontId::Display, area.Right() - scoreWidth, y + (rowHeight - p.LineHeight(FontId::Display)) / 2, score,
               toMove || winner ? TextColor : ui::Fade(TextColor, 0.7f));

        // The name in the room left of the score, and the computer's level where it fits too.
        const float textX = area.x + 28 * scale, room = area.Right() - scoreWidth - 10 * scale - textX;
        std::string name = Name(side);
        if (!IsHuman(side)) {
            std::string withLevel = std::format("{}  ·  {}", name, T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)));
            if (p.Measure(FontId::Body, withLevel) <= room)
                name = std::move(withLevel);
        }
        const float bodyHeight = p.LineHeight(FontId::Body);
        p.Text(FontId::Body, textX, y + rowHeight / 2 - bodyHeight + 1 * scale, p.Elided(FontId::Body, name, room),
               toMove || winner ? TextColor : ui::Fade(TextColor, 0.8f));
        const int pips = nardy::Pips(m_board, side, GameVariant());
        const int off = m_board.off[static_cast<std::size_t>(side)];
        p.Text(FontId::Body, textX, y + rowHeight / 2 + 1 * scale, p.Elided(FontId::Body, std::vformat(T(Str::Pips), std::make_format_args(pips, off)), room),
               MutedColor);
    }
}

void App::DrawHistory(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill(area, 12 * scale, Rgba(0, 0, 0, 40));
    if (m_history.empty()) {
        p.TextCentered(FontId::Body, area, T(Str::NoTurnsYet), MutedColor);
        return;
    }
    const Rect inner = area.Inset(9 * scale);
    struct Line {
        std::string text;
        int kind;
        bool first;
    };
    std::vector<Line> lines;
    const std::size_t start = m_history.size() > 120 ? m_history.size() - 120 : 0;
    for (std::size_t i = start; i < m_history.size(); ++i) {
        bool first = true;
        for (std::string& line : WrapText(FontId::Body, m_history[i].text, inner.width - 16 * scale)) {
            lines.push_back({std::move(line), m_history[i].kind, first});
            first = false;
        }
    }
    const float rowHeight = std::round(22 * scale);
    const int visible = std::max(1, static_cast<int>(inner.height / rowHeight));
    const float maxScroll = static_cast<float>(std::max(0, static_cast<int>(lines.size()) - visible));
    if (area.Contains(m_pointer.x, m_pointer.y) && m_pointer.wheel != 0 && !DialogOpen()) {
        m_historyScroll = std::clamp(m_historyScroll - m_pointer.wheel * 3, 0.0f, maxScroll);
        m_followHistory = m_historyScroll >= maxScroll;
        m_pointer.wheel = 0;
    }
    if (m_followHistory)
        m_historyScroll = maxScroll;
    m_historyScroll = std::clamp(m_historyScroll, 0.0f, maxScroll);
    const ui::Color accent = ui::FromLinear(Accent());
    const Theme& theme = Themes()[static_cast<std::size_t>(m_theme)];
    const int first = static_cast<int>(m_historyScroll);
    for (int i = first; i < std::min(static_cast<int>(lines.size()), first + visible); ++i) {
        const Line& line = lines[static_cast<std::size_t>(i)];
        const float y = inner.y + static_cast<float>(i - first) * rowHeight;
        const ui::Color color = line.kind == 2 ? NoteColor : line.kind == 3 ? accent : TextColor;
        if (line.first) {
            if (line.kind <= 1)
                p.Disc(inner.x + 4 * scale, y + rowHeight / 2, 3.5f * scale,
                       ui::FromLinear(theme.checkers[static_cast<std::size_t>(line.kind)].color * 1.25f + Vec3{0.03f, 0.03f, 0.03f}));
            else
                p.Disc(inner.x + 4 * scale, y + rowHeight / 2, 2.5f * scale, ui::Fade(color, 0.7f));
        }
        p.Text(FontId::Body, inner.x + 16 * scale, y + (rowHeight - p.LineHeight(FontId::Body)) / 2, line.text,
               i == static_cast<int>(lines.size()) - 1 || line.kind >= 2 ? color : ui::Fade(color, 0.82f));
    }
    if (maxScroll > 0) {
        const float barHeight = inner.height * static_cast<float>(visible) / static_cast<float>(lines.size());
        const float barY = inner.y + (inner.height - barHeight) * (m_historyScroll / maxScroll);
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
    if (panel.Contains(m_pointer.x, m_pointer.y) && (m_pointer.leftPressed || m_pointer.rightPressed) && m_gesture == Gesture::None)
        m_pointer.consumed = true;
    p.Shadow(panel, 22 * scale, Rgba(0, 0, 0, 70));
    p.Fill(panel, 18 * scale, Rgba(16, 18, 26, 120));
    p.Outline(panel, 18 * scale, Rgba(255, 255, 255, 24));

    // In a low window everything is packed closer, and the footer goes.
    const bool compact = panel.height < 820 * scale;
    const float pad = (compact ? 16 : 20) * scale;
    const float x = panel.x + pad, width = panel.width - 2 * pad;
    float y = panel.y + pad;

    const float small = 30 * scale;
    p.Text(FontId::Heading, x, y, p.Elided(FontId::Heading, T(Str::Title), width - 2 * small - 14 * scale), TextColor);
    if (Button(HelpButton, {panel.Right() - pad - small, y - 2 * scale, small, small}, "?"))
        ToggleDialog(Dialog::Help);
    if (Button(AboutButton, {panel.Right() - pad - 2 * small - 6 * scale, y - 2 * scale, small, small}, "i"))
        ToggleDialog(Dialog::About);
    y += (compact ? 38 : 44) * scale;

    const float statusHeight = (compact ? 54 : 62) * scale;
    DrawStatus({x, y, width, statusHeight});
    y += statusHeight + (compact ? 6 : 10) * scale;

    const float playersHeight = 92 * scale;
    DrawPlayers({x + 4 * scale, y, width - 8 * scale, playersHeight});
    y += playersHeight + (compact ? 6 : 10) * scale;

    // The buttons of the turn.
    const float rowHeight = (compact ? 29 : 34) * scale, gap = (compact ? 5 : 8) * scale;
    const float half = (width - gap) / 2;
    const float buttonHeight = (compact ? 31 : 38) * scale;
    const bool canRoll = m_stage == Stage::ToRoll && IsHuman(m_toPlay);
    if (Button(RollButton, {x, y, half, buttonHeight}, T(Str::Roll), canRoll, canRoll))
        RollDice();
    const bool canHint = HumanToMove() && !m_sequences.empty() && m_doneClock <= 0 && !m_hintWanted;
    if (Button(HintButton, {x + half + gap, y, half, buttonHeight}, T(Str::Hint), canHint, m_hint.has_value() && canHint))
        RequestHint();
    y += buttonHeight + gap;
    const bool earlier = std::ranges::any_of(m_undo, [&](const Snapshot& snapshot) { return snapshot.turn < m_turn || m_stage == Stage::Over; });
    const bool canUndo = m_mode != Demo && ((HumanToMove() && !m_played.empty()) || earlier);
    if (Button(UndoButton, {x, y, half, buttonHeight}, T(Str::Undo), canUndo))
        Undo();
    if (Button(NewGameButton, {x + half + gap, y, half, buttonHeight}, T(Str::NewGame), true, m_stage == Stage::Over))
        NewGame();
    y += buttonHeight + (compact ? 8 : 12) * scale;

    // From the bottom up: the footer, the switches and the settings; the moves take the space between.
    float bottom = panel.Bottom() - pad;
    if (!compact) {
        const float footerHeight = p.LineHeight(FontId::Body);
        // Into the panel's padding where the desktop's font is wide.
        const float room = panel.width - 16 * scale;
        p.TextCentered(FontId::Body, {panel.x, bottom - footerHeight, panel.width, footerHeight}, p.Elided(FontId::Body, T(Str::Footer), room),
                       ui::Fade(MutedColor, 0.8f));
        bottom -= footerHeight + 14 * scale;
    }

    bottom -= rowHeight;
    if (Button(SoundButton, {x, bottom, half, rowHeight}, T(Str::Sound), true, m_sound.Enabled()))
        ToggleSound();
    if (Button(LanguageButton, {x + half + gap, bottom, half, rowHeight}, T(Str::LanguageName)))
        ToggleLanguage();
    bottom -= gap + rowHeight;
    if (Button(AutoRollButton, {x, bottom, half, rowHeight}, T(Str::AutoRoll), true, m_autoRoll))
        SetAutoRoll(!m_autoRoll);
    static constexpr std::array<Str, CameraModeCount> cameras{Str::CameraOverview, Str::CameraTop};
    const std::string view = std::format("{}: {}", T(Str::Camera), T(cameras[static_cast<std::size_t>(m_camera.Mode())]));
    if (Button(CameraButton, {x + half + gap, bottom, half, rowHeight}, view))
        SetCamera(static_cast<int>(m_camera.Mode()) + 1);
    bottom -= gap;

    static constexpr std::array<Str, 3> modes{Str::ModeComputer, Str::ModeTwoPlayers, Str::ModeDemo};
    static constexpr std::array<Str, 2> games{Str::GameLong, Str::GameShort};
    const Theme& theme = Themes()[static_cast<std::size_t>(m_theme)];
    struct Choice {
        int id;
        Str label;
        std::string value;
    };
    const std::array<Choice, 4> choices{{
        {ThemeSelector, Str::Theme, std::string(m_language == Language::Russian ? theme.nameRussian : theme.nameEnglish)},
        {LevelSelector, Str::Level, std::format("{} · {}", m_level, T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)))},
        {ModeSelector, Str::Mode, std::string(T(modes[static_cast<std::size_t>(m_mode)]))},
        {GameSelector, Str::Game, std::string(T(games[static_cast<std::size_t>(m_game)]))},
    }};
    float labelWidth = 0;
    for (const Choice& choice : choices)
        labelWidth = std::max(labelWidth, p.Measure(FontId::Body, T(choice.label)));
    for (const Choice& choice : choices) {
        bottom -= rowHeight;
        if (const int change = Selector(choice.id, {x, bottom, width, rowHeight}, T(choice.label), choice.value, labelWidth); change != 0) {
            switch (choice.id) {
            case ThemeSelector:
                SetTheme(m_theme + change);
                break;
            case LevelSelector:
                SetLevel(m_level + change);
                break;
            case ModeSelector:
                SetMode(m_mode + change);
                break;
            default:
                SetGame(m_game + change);
                break;
            }
        }
        bottom -= (compact ? 3 : 6) * scale;
    }
    bottom -= (compact ? 6 : 10) * scale;

    // The moves so far, where there is room for them.
    const float labelHeight = p.LineHeight(FontId::Body) + 6 * scale;
    if (bottom - y - labelHeight > 36 * scale) {
        p.Text(FontId::Body, x, y, T(Str::History), MutedColor);
        DrawHistory({x, y + labelHeight, width, bottom - y - labelHeight});
    }
}

void App::DrawBanner()
{
    if (m_bannerFade <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect card = BannerCard();
    p.Shadow(card, 24 * scale, ui::Fade(Rgba(0, 0, 0, 120), m_bannerFade));
    p.Fill(card, 18 * scale, ui::Fade(Rgba(14, 16, 24, 150), m_bannerFade));
    p.Outline(card, 18 * scale, ui::Fade(ui::FromLinear(Accent()), 0.6f * m_bannerFade));
    p.TextCentered(FontId::Display, {card.x, card.y + 8 * scale, card.width, 48 * scale}, m_bannerTitle, ui::Fade(TextColor, m_bannerFade));
    p.TextCentered(FontId::Body, {card.x, card.y + 54 * scale, card.width, 26 * scale}, m_bannerDetail, ui::Fade(MutedColor, m_bannerFade));
}

// The card's glass is frosted over the scene; over the panel, where there is nothing to frost, it is darker instead.
void App::DrawCard(Rect card)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const bool overPanel = m_panelSlide > 0.001f && card.Right() > PanelRect().x;
    p.Shadow(card, 26 * scale, Rgba(0, 0, 0, 140));
    p.Fill(card, 20 * scale, Rgba(18, 20, 28, overPanel ? 235 : 175));
    p.Outline(card, 20 * scale, Rgba(255, 255, 255, 30));
}

void App::DrawHelp()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));

    const HelpLayout layout = LayOutHelp();
    const Rect card = CenteredCard(layout.width, layout.height);
    const float pad = HelpPad * scale, rowHeight = layout.rowHeight;
    const ui::Color accent = ui::FromLinear(Accent());
    DrawCard(card);
    p.Text(FontId::Heading, card.x + pad, card.y + pad - 4 * scale, T(Str::Controls), TextColor);
    const float top = card.y + pad + 44 * scale;
    const auto lines = HelpLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t column = i / HelpRows;
        const float x = card.x + pad + (column == 0 ? 0.0f : layout.second);
        const float y = top + static_cast<float>(i % HelpRows) * rowHeight;
        p.Text(FontId::Body, x, y, lines[i].keys, accent);
        p.Text(FontId::Body, x + layout.keys[column], y, T(lines[i].action), TextColor);
    }
    // The rules of the game on the board, in a few sentences.
    float y = top + static_cast<float>(HelpRows) * rowHeight + 12 * scale;
    p.Text(FontId::Body, card.x + pad, y, T(Str::Rules), accent);
    for (const std::string& line : layout.rules) {
        y += rowHeight;
        p.Text(FontId::Body, card.x + pad, y, line, MutedColor);
    }
    if (Button(HelpClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") || PressedOutside(card))
        m_dialog = Dialog::None;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
}

void App::DrawAbout()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));
    const Rect card = AboutCard();
    const float pad = AboutPad * scale;
    const ui::Color accent = ui::FromLinear(Accent());
    DrawCard(card);

    // The icon, and beside it the name and the version.
    const float x = card.x + pad, width = card.width - 2 * pad, icon = AboutIcon * scale;
    float y = card.y + pad;
    p.Icon({x, y, icon, icon});
    const float titleHeight = p.LineHeight(FontId::Display), versionHeight = p.LineHeight(FontId::Body);
    const float titleY = y + (icon - titleHeight - versionHeight) / 2;
    p.Text(FontId::Display, x + icon + 18 * scale, titleY, "3D Nardy", TextColor);
    p.Text(FontId::Body, x + icon + 18 * scale, titleY + titleHeight, "Version " NARDY3D_VERSION " for Linux", MutedColor);
    y += icon + 20 * scale;

    for (const std::string& line : WrapText(FontId::Body, AboutDescription, width)) {
        p.Text(FontId::Body, x, y, line, TextColor);
        y += p.LineHeight(FontId::Body) + 3 * scale;
    }
    y += 16 * scale;
    p.Text(FontId::Heading, x, y, "Created by Mykhailo Makarov", TextColor);
    y += p.LineHeight(FontId::Heading) + 2 * scale;
    p.Text(FontId::Body, x, y, "© 2026 · MIT License", MutedColor);
    y += p.LineHeight(FontId::Body) + 20 * scale;

    // The author's pages, opened in the browser.
    float nameWidth = 0;
    for (const Link& link : AuthorLinks)
        nameWidth = std::max(nameWidth, p.Measure(FontId::Body, link.name));
    for (std::size_t i = 0; i < AuthorLinks.size(); ++i) {
        const Link& link = AuthorLinks[i];
        const Rect row{x, y, width, LinkRow * scale};
        const Interaction state = Interact(AboutLinks + static_cast<int>(i), row, true);
        p.Fill(row, 9 * scale, state.pressed ? Rgba(255, 255, 255, 52) : state.hover ? Rgba(255, 255, 255, 34) : Rgba(255, 255, 255, 12));
        const float textY = row.y + (row.height - p.LineHeight(FontId::Body)) / 2;
        p.Text(FontId::Body, row.x + 14 * scale, textY, link.name, TextColor);
        p.Text(FontId::Body, row.x + 32 * scale + nameWidth, textY, link.address, accent);
        p.Text(FontId::Body, row.Right() - 14 * scale - p.Measure(FontId::Body, "↗"), textY, "↗", state.hover ? TextColor : MutedColor);
        if (state.clicked)
            OpenLink(link.url);
        y += (LinkRow + LinkGap) * scale;
    }

    if (Button(AboutClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") || PressedOutside(card))
        m_dialog = Dialog::None;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
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
