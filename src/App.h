#pragma once

#include "Ai.h"
#include "BoardView.h"
#include "Camera.h"
#include "Gl.h"
#include "Nardy.h"
#include "Network.h"
#include "Renderer.h"
#include "Resources.h"
#include "Settings.h"
#include "Sound.h"
#include "Strings.h"
#include "Text.h"
#include "UiPainter.h"

#include <SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// The game's window: it runs the games, the computer players and the interface, and draws them.
class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run();

private:
    enum Mode { VsComputer, TwoPlayers, Demo };

    // The players throw a die each to see who starts; then each turn the dice are thrown and fly, and the checkers
    // are moved: by hand, or by the computer, which thinks and then plays its steps one by one. Then the game is over.
    enum class Stage { Opening, ToRoll, Rolling, Moving, Thinking, Playing, Over };

    // SDL from the first to the last moment of the game; everything else lives within it.
    struct SdlSession {
        SdlSession();
        ~SdlSession();
        SdlSession(const SdlSession&) = delete;
        SdlSession& operator=(const SdlSession&) = delete;
    };

    struct WindowDeleter {
        void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
    };
    struct CursorDeleter {
        void operator()(SDL_Cursor* cursor) const { SDL_FreeCursor(cursor); }
    };

    // What the mouse did since the last frame, in pixels.
    struct Pointer {
        float x = -1, y = -1;
        bool leftDown = false, rightDown = false;
        bool leftPressed = false, leftReleased = false, rightPressed = false, rightReleased = false;
        bool doubleClick = false;
        float wheel = 0;
        bool consumed = false; // taken by the interface
    };

    enum class Gesture { None, Press, Drag, TurnCamera };

    // The card shown over the game, which takes the mouse and the keys while it is open.
    enum class Dialog { None, Help, About };

    // What the pointer does to a widget in this frame.
    struct Interaction {
        bool hover = false, pressed = false, clicked = false;
    };

    struct Toast {
        std::string text;
        float age = 0;
    };

    // A human's turn as it began, with its dice: what taking back a turn returns to.
    struct Snapshot {
        nardy::Position board;
        int toPlay = 0;
        std::array<bool, 2> firstMove{};
        nardy::Roll roll;
        std::size_t history = 0;
        int turn = 0;
    };

    struct HistoryLine {
        std::string text;
        int kind = 0; // 0 White's turn, 1 Black's, 2 a note, 3 the end of the game
    };

    // A way for the checker chosen to reach a place: the steps, one checker's, in order.
    struct Route {
        int to = 0; // in the mover's numbers: a point or BearOff
        std::vector<nardy::Step> steps;
    };

    // The window's size when it is neither maximized nor full screen, in window units.
    struct WindowSize {
        int width = 0, height = 0;
    };

    // A line of the help: the keys, and what they do.
    struct HelpLine {
        std::string_view keys;
        Str action;
    };
    static constexpr int HelpRows = 11; // in each of the help's two columns

    // The help's card: its two columns as wide as the desktop's font needs them, and its rows closer together where
    // the window is too low for them.
    struct HelpLayout {
        float width = 0, height = 0;    // of the card
        float second = 0;               // from the card's padding to the second column
        std::array<float, 2> keys{};    // the width of the keys in each column, before the actions
        float rowHeight = 0;
        std::vector<std::string> rules; // the rules of the game on the board, in lines
    };

    void CreateMainWindow(const resources::Image& icon);
    void HandleEvent(const SDL_Event& event);
    void HandleWindowEvent(const SDL_WindowEvent& event);
    void UpdateSize();
    void ShowLoading();
    void BuildFonts();
    void LoadNetworks();
    void Frame();
    void Update(float seconds);
    void PlaySounds();
    bool NeedsAnimation() const;
    float BannerTarget() const; // 1 while the banner is to show, else 0
    render::Scene BuildScene() const;

    // The game.
    nardy::Variant GameVariant() const { return m_game == 0 ? nardy::Variant::Long : nardy::Variant::Short; }
    const nardy::Network& Net() const { return m_nets[static_cast<std::size_t>(m_game)]; }
    bool IsHuman(int side) const;
    bool HumanToMove() const { return m_stage == Stage::Moving && IsHuman(m_toPlay); }
    // The board as the player to move sees it, and back.
    nardy::Position MoverView() const;
    void SetFromMover(const nardy::Position& mover);
    // A place in the mover's numbers (a point, FromBar or BearOff) as BoardView counts places, and back.
    int ToPlace(int point) const;
    int FromPlace(int place) const;
    bool LoadGame(const std::string& text);
    std::string SaveGame() const;
    void NewGame();
    void ThrowOpening();
    void FinishOpening();
    void StartTurn();
    void RollDice();
    void BeginMoves();
    void PlayStep(const nardy::Step& step);
    void TakeBackStep();
    void TakeBackTurn();
    void Undo();
    void EndTurn();
    void FinishGame();
    void StopComputer();
    void UpdateComputer(float seconds);
    void RequestHint();
    void UpdateHint();
    // The steps that may be played next, and the routes from a place in the mover's numbers.
    std::vector<nardy::Step> NextSteps() const;
    std::vector<Route> RoutesFrom(int point) const;
    void PlayRoute(const Route& route);
    bool PlayLargerDie(int point);
    void Select(int place);
    // The checker carried or chosen goes back to its place, and is let go of.
    void DropSelection();
    std::string WhyNot(int point) const;
    void SetGame(int game);
    void SetMode(int mode);
    void SetLevel(int level);
    void SetTheme(int theme);
    void SetCamera(int camera);
    void SetAutoRoll(bool on);
    void ToggleSound();
    void ToggleLanguage();
    void ToggleFullScreen();
    void OpenLink(const char* url);
    void Save();
    void AddHistory(std::string text, int kind);
    std::string MoveText(const std::vector<nardy::Step>& steps) const;

    // Input.
    void PointerAt(int windowX, int windowY);
    void HandleBoardPointer();
    void HandleKey(const SDL_KeyboardEvent& event);
    bool PointerRay(math::Vec3& origin, math::Vec3& direction) const;
    std::optional<math::Vec3> PointerOnPlane(float height) const;
    // The place under the pointer, as BoardView counts places, or -1.
    int PlaceUnderPointer() const;

    // The interface.
    float Scale() const { return m_scale; }
    bool DialogOpen() const { return m_dialog != Dialog::None; }
    ui::Rect PanelRect() const;
    ui::Rect FreeArea() const;
    std::vector<render::FrostedPanel> FrostedPanels() const;
    // The text in lines no wider than `width`, broken at the spaces.
    std::vector<std::string> WrapText(ui::FontId font, std::string_view text, float width) const;
    std::array<HelpLine, 2 * HelpRows> HelpLines() const;
    HelpLayout LayOutHelp() const;
    // A card of the size over the game: beside the panel, or over it where the window is too narrow for that.
    ui::Rect CenteredCard(float width, float height) const;
    ui::Rect HelpCard() const;
    ui::Rect AboutCard() const;
    ui::Rect BannerCard() const;
    void DrawInterface();
    void DrawPanel();
    void DrawStatus(ui::Rect area);
    void DrawPlayers(ui::Rect area);
    void DrawHistory(ui::Rect area);
    void DrawBanner();
    // The background of a card over the game.
    void DrawCard(ui::Rect card);
    void DrawHelp();
    void DrawAbout();
    void DrawToasts();
    void DrawChecker(float x, float y, float radius, int side);
    void DrawDie(float x, float y, float size, int value, float dim);
    void ToggleDialog(Dialog dialog);
    // A press outside the open card closes it, unless it is the click that opened it.
    bool PressedOutside(ui::Rect card) const;
    // Hovering, pressing and clicking; a click sounds.
    Interaction Interact(int id, ui::Rect rect, bool enabled);
    bool Button(int id, ui::Rect rect, std::string_view label, bool enabled = true, bool accent = false);
    // A label with a value stepped by two arrows, past the width of the longest label of the column; returns -1 or 1
    // when an arrow is clicked, else 0.
    int Selector(int id, ui::Rect rect, std::string_view label, std::string_view value, float labelWidth);
    void ShowToast(std::string text);
    void ShowBanner(std::string title, std::string detail, float seconds);
    std::string_view T(Str id) const { return Tr(id, m_language); }
    // The words for one side: "you" or "the computer" against the computer, else "White" or "Black".
    std::string_view Pick(int side, Str you, Str computer, Str white, Str black) const;
    std::string Name(int side) const;
    std::string StatusTitle() const;
    std::string StatusDetail() const;
    math::Vec3 Accent() const;

    // Declared in the order of their making; destroyed the other way round, the window after what draws in it.
    SdlSession m_sdl;
    SoundBank m_sound;
    std::unique_ptr<SDL_Window, WindowDeleter> m_window;
    std::unique_ptr<SDL_Cursor, CursorDeleter> m_arrowCursor, m_handCursor;
    std::unique_ptr<gl::Context> m_context;
    std::unique_ptr<render::Renderer> m_renderer;
    std::unique_ptr<ui::Painter> m_painter;
    std::array<std::unique_ptr<text::Font>, 3> m_fonts;

    Settings m_settings;
    Language m_language = Language::English;
    int m_width = 0, m_height = 0;             // of the picture, in pixels
    int m_windowWidth = 0, m_windowHeight = 0; // in window units, which the mouse reports
    float m_textScale = 1;                     // the desktop's
    float m_scale = 1;                         // of the interface: the desktop's, in pixels
    float m_fontScale = 0;                     // that the fonts were made for
    bool m_running = true;
    bool m_dirty = true; // something to draw since the last frame
    bool m_fullScreen = false;
    bool m_maximizedBeforeFullScreen = false;
    WindowSize m_normalSize, m_previousNormalSize;
    std::uint32_t m_normalSizeTime = 0; // when the window last took a size of its own, in SDL's milliseconds
    bool m_cursorHand = false, m_cursorShownHand = false;
    std::chrono::steady_clock::time_point m_lastFrame;
    float m_time = 0;

    // The game.
    std::array<nardy::Network, 2> m_nets; // long and short nardy
    nardy::Position m_board;              // as White sees it
    int m_toPlay = 0;
    std::array<bool, 2> m_firstMove{true, true};
    nardy::Roll m_roll;
    bool m_rolled = false;
    std::array<int, 2> m_opening{}; // the dice thrown to see who starts
    nardy::Position m_turnStart;
    std::vector<nardy::Step> m_played;    // this turn's steps so far, in the mover's numbers
    std::vector<nardy::Move> m_sequences; // every legal way to play this turn's roll
    std::vector<int> m_lastMove;          // the places the last turn moved from and to, as BoardView counts them
    std::array<int, 2> m_score{};
    int m_winner = -1;
    int m_winPoints = 0;
    Stage m_stage = Stage::Opening;
    float m_stageClock = 0;
    float m_doneClock = -1; // counts down after the last step of a turn, before it ends
    int m_mode = VsComputer;
    int m_level = 3;
    int m_theme = 0;
    int m_game = 0;
    bool m_autoRoll = true;
    int m_turn = 0;
    std::mt19937_64 m_random;
    std::vector<Snapshot> m_undo;
    std::vector<HistoryLine> m_history;
    BoardView m_view;

    // The computer.
    nardy::Worker<nardy::Move> m_computer;
    std::optional<nardy::Move> m_computerMove;
    int m_computerStep = 0;
    float m_stepClock = 0;
    nardy::Worker<nardy::Move> m_hintWorker;
    bool m_hintWanted = false;
    std::optional<nardy::Move> m_hint;

    // The view and the input.
    BoardCamera m_camera;
    Pointer m_pointer;
    math::Vec2 m_drawnPointer{-1, -1}; // where the pointer was when the last frame was drawn
    Gesture m_gesture = Gesture::None;
    float m_pressX = 0, m_pressY = 0;
    float m_dragX = 0, m_dragY = 0;
    bool m_pressRight = false;
    bool m_pressDouble = false;
    int m_pressPlace = -1;
    int m_toggledPlace = -1; // where the last click chose or let go of a checker, for a double-click
    int m_selected = -1;     // the place chosen to move from, as BoardView counts places
    int m_hoverPlace = -1;

    // The interface.
    int m_activeWidget = 0;
    bool m_panelVisible = true;
    float m_panelSlide = 1;
    Dialog m_dialog = Dialog::None;
    bool m_dialogOpening = false; // opened in this frame
    float m_historyScroll = 0;
    bool m_followHistory = true;
    float m_bannerFade = 0;
    float m_bannerTime = 0; // left to show; below zero it stays
    std::string m_bannerTitle, m_bannerDetail;
    std::vector<Toast> m_toasts;
};
