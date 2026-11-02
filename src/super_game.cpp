#include "super_game.h"

#include <cstdio>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "color_vision.h"
#include "expanded_edition.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "two_towers.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#include <SDL.h>

namespace rotk {
namespace {

namespace fs = std::filesystem;

// "ttt-rotk-sprites": The Two Towers draws its heroes with ROTK's sprites (src/rotk_sprites.cpp in its
// host), read from this game's ROM, saved once per session where it can find it.
bool g_tttRotkSprites = false;
fs::path g_rotkRomCopy;
constexpr std::uint32_t kRomBytes = 0x02000000u;

// "ttt-rotk-portraits": its dialog portraits are ROTK's (same ROM copy).
bool g_tttRotkPortraits = false;
// "ttt-rotk-sounds": some of its sound effects are ROTK's (src/rotk_sounds.cpp in its host).
bool g_tttRotkSounds = false;
// "ttt-rotk-hud": its HUD is ROTK's (src/rotk_hud.cpp in its host).
bool g_tttRotkHud = false;

void tttRotkSpritesActivated() { g_tttRotkSprites = true; }
void tttRotkPortraitsActivated() { g_tttRotkPortraits = true; }
void tttRotkSoundsActivated() { g_tttRotkSounds = true; }
void tttRotkHudActivated() { g_tttRotkHud = true; }
void tttRotkSpritesReset()
{
    g_tttRotkSprites = std::getenv("ROTK_TTT_ROTK_SPRITES") != nullptr;
    g_tttRotkPortraits = std::getenv("ROTK_TTT_ROTK_PORTRAITS") != nullptr;
    g_tttRotkSounds = std::getenv("ROTK_TTT_ROTK_SOUNDS") != nullptr;
    g_tttRotkHud = std::getenv("ROTK_TTT_ROTK_HUD") != nullptr;
}

// This game's ROM as loaded, in a temp file for The Two Towers' host.
fs::path rotkRomCopy()
{
    if (!g_rotkRomCopy.empty()) return g_rotkRomCopy;
    const fs::path path = fs::temp_directory_path() / "rotk-recompiled-rom.gba";
    std::ofstream file(path, std::ios::binary);
    std::vector<char> bytes(kRomBytes);
    for (std::uint32_t i = 0; i < kRomBytes; i += 4) {
        const std::uint32_t v = bus_read_u32(0x08000000u + i);
        std::memcpy(bytes.data() + i, &v, 4);
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file) g_rotkRomCopy = path;
    return g_rotkRomCopy;
}

constexpr std::uint32_t kSwitch = 1u << 1;  // PC_HOST_SUPER_GAME_SWITCH
constexpr const char* kExecutable = "TTT-Recompiled.exe";
constexpr unsigned kPumpMs = 50;
constexpr int kMaxSdlWindowId = 16;
constexpr int kMaxAudioDeviceId = 16;

fs::path g_executable, g_rom, g_bios, g_save;

// The launcher row's verdict on the last path it asked about, kept so the file is read once.
std::string g_checkedPath;
bool g_checkedValid = false;

bool checkedValid(const char* path)
{
    if (!path || !*path) return false;
    if (g_checkedPath != path) {
        g_checkedPath = path;
        g_checkedValid = twoTowersRomValid(path);
    }
    return g_checkedValid;
}

const char* companionStatus(const char* path)
{
    if (!path || !*path) return nullptr;
    return checkedValid(path) ? "The Two Towers (USA, Europe): ready"
                              : "Not The Two Towers (USA, Europe), game code ALPE: pick another ROM";
}

void companionChosen(const char* path)
{
    if (!checkedValid(path)) return;
#if defined(_WIN32)
    SetEnvironmentVariableA("ROTK_PC_TWO_TOWERS_ROM", path);
#else
    setenv("ROTK_PC_TWO_TOWERS_ROM", path, 1);
#endif
}

std::string argValue(const std::vector<std::string>& args, const char* name)
{
    for (std::size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return {};
}

// --bios, else the [bios] path in the --config file, else gba_bios.bin beside the ROM or this executable.
fs::path findBios(const std::vector<std::string>& args)
{
    if (std::string bios = argValue(args, "--bios"); !bios.empty()) return bios;
    std::ifstream config(argValue(args, "--config"));
    std::string line, section;
    while (std::getline(config, line)) {
        if (!line.empty() && line[0] == '[') section = line;
        const auto quote = line.find('"');
        if (section == "[bios]" && line.rfind("path", 0) == 0 && quote != std::string::npos)
            return line.substr(quote + 1, line.find('"', quote + 1) - quote - 1);
    }
    std::error_code error;
    const fs::path rom = sessionRomPath(args);
    for (const fs::path& folder : {rom.empty() ? fs::path() : fs::absolute(rom).parent_path(), executableFolder()})
        if (!folder.empty() && fs::is_regular_file(folder / "gba_bios.bin", error)) return folder / "gba_bios.bin";
    return {};
}

// ROTK_TTT_EXE, else beside this executable, else the development build's folder.
fs::path findExecutable()
{
    std::error_code error;
    if (const char* given = std::getenv("ROTK_TTT_EXE"); given && fs::is_regular_file(given, error)) return given;
    for (const fs::path& candidate :
         {executableFolder() / kExecutable, executableFolder() / ".." / "host-ttt" / kExecutable})
        if (fs::is_regular_file(candidate, error)) return fs::weakly_canonical(candidate, error);
    return {};
}

// The game's window: the SDL window that is shown.
SDL_Window* gameWindow()
{
    for (int id = 1; id <= kMaxSdlWindowId; ++id)
        if (SDL_Window* window = SDL_GetWindowFromID(static_cast<Uint32>(id)))
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) return window;
    return nullptr;
}

std::string quoted(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    return "\"" + arg + "\"";
}

#if defined(_WIN32)
// The Two Towers runs beside this game for the whole session (started hidden on its first frames,
// see src/plain_main.cpp), so a switch is a window swap with a fade, both ways. Events and files are
// named after this process.
struct Companion {
    PROCESS_INFORMATION process{};
    bool running = false;
    int failures = 0;
    HANDLE job = nullptr;  // ends it with this game
    HANDLE show = nullptr, ready = nullptr, back = nullptr, saved = nullptr;
    fs::path showFile, backFile;
    bool savePending = false;
} g_companion;

// The fade: this many frames each way.
constexpr int kFadeFrames = 8;
int g_fadeOut = -1;  // frames left until the swap, -1 when not fading out
int g_fadeIn = 0;    // frames left of the fade in
// how long a switch waits for The Two Towers to show (it may still be booting) before giving up
constexpr unsigned kShowTimeoutMs = 20000;

std::wstring eventName(const wchar_t* what)
{
    return std::wstring(L"Local\\RotkSuperGame") + what + std::to_wstring(GetCurrentProcessId());
}

void makeEvents()
{
    Companion& c = g_companion;
    if (c.show) return;
    c.show = CreateEventW(nullptr, FALSE, FALSE, eventName(L"Show").c_str());
    c.ready = CreateEventW(nullptr, FALSE, FALSE, eventName(L"Ready").c_str());
    c.back = CreateEventW(nullptr, FALSE, FALSE, eventName(L"Back").c_str());
    c.saved = CreateEventW(nullptr, FALSE, FALSE, eventName(L"Saved").c_str());
    const std::string pid = std::to_string(GetCurrentProcessId());
    c.showFile = fs::temp_directory_path() / ("rotk-super-game-" + pid + "-show.txt");
    c.backFile = fs::temp_directory_path() / ("rotk-super-game-" + pid + ".txt");
    c.job = CreateJobObjectW(nullptr, nullptr);
    if (c.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(c.job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
}

bool companionAlive()
{
    Companion& c = g_companion;
    if (!c.running) return false;
    if (WaitForSingleObject(c.process.hProcess, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(c.process.hThread);
    CloseHandle(c.process.hProcess);
    c.process = {};
    c.running = false;
    // it quit (its window closed): its save was written on the way out
    twoTowersReloadSave();
    c.savePending = false;
    return false;
}

std::string windowPlace(SDL_Window* window)
{
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = 0, h = 0;
    bool fullscreen = false;
    if (window) {
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    }
    char place[96];
    std::snprintf(place, sizeof place, "%d,%d,%d,%d,%d", x, y, w, h, fullscreen ? 1 : 0);
    return place;
}

// Starts The Two Towers hidden; it boots to its main menu and waits there.
bool startCompanion()
{
    Companion& c = g_companion;
    if (companionAlive()) return true;
    makeEvents();
    std::error_code error;
    fs::remove(c.backFile, error);
    fs::remove(c.showFile, error);
    ResetEvent(c.show);
    ResetEvent(c.ready);
    ResetEvent(c.back);
    ResetEvent(c.saved);
    SetEnvironmentVariableA("GBARECOMP_WINDOW_HANDOFF", windowPlace(gameWindow()).c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_SHOW", eventName(L"Show").c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_READY", eventName(L"Ready").c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_BACK", eventName(L"Back").c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_SAVED", eventName(L"Saved").c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_SHOW_FILE", c.showFile.wstring().c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_BACK_FILE", c.backFile.wstring().c_str());
    SetEnvironmentVariableA("ROTK_SUPER_GAME_PID", std::to_string(GetCurrentProcessId()).c_str());
    // ROTK's mod selection (GBARECOMP_MODS_ROOT, set by the launcher) is ROTK's code: The Two Towers
    // keeps its own. CPU boost and stereo carry over through their own variables.
    wchar_t modsRoot[MAX_PATH] = {};
    const bool hadModsRoot = GetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", modsRoot, MAX_PATH) != 0;
    SetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", nullptr);
    // Display features the plain host has carry over as ROTK has them set now.
    char colorVision[16] = {};
    const bool hadColorVision = GetEnvironmentVariableA("ROTK_PC_COLOR_VISION", colorVision, sizeof colorVision) != 0;
    SetEnvironmentVariableA("ROTK_PC_COLOR_VISION", colorVisionModeName());
    const fs::path rotkRom = g_tttRotkSprites || g_tttRotkPortraits || g_tttRotkSounds || g_tttRotkHud ? rotkRomCopy() : fs::path();
    SetEnvironmentVariableA("ROTK_TTT_ROTK_SPRITES", !rotkRom.empty() && g_tttRotkSprites ? "1" : nullptr);
    SetEnvironmentVariableA("ROTK_TTT_ROTK_PORTRAITS", !rotkRom.empty() && g_tttRotkPortraits ? "1" : nullptr);
    SetEnvironmentVariableA("ROTK_TTT_ROTK_SOUNDS", !rotkRom.empty() && g_tttRotkSounds ? "1" : nullptr);
    SetEnvironmentVariableA("ROTK_TTT_ROTK_HUD", !rotkRom.empty() && g_tttRotkHud ? "1" : nullptr);
    SetEnvironmentVariableA("ROTK_TTT_ROTK_ROM", rotkRom.empty() ? nullptr : rotkRom.string().c_str());

    // straight into the game: the GBA boot logo would break the swap
    std::string command = quoted(g_executable.string()) + " --window --bios-skip-intro --rom " + quoted(g_rom.string()) +
                          " --save-path " + quoted(g_save.string());
    if (!g_bios.empty()) command += " --bios " + quoted(g_bios.string());
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    // It runs in this game's folder, so it shares the control bindings (keybinds.ini, input.ini).
    c.running = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                               &c.process) != 0;
    if (c.running && c.job) AssignProcessToJobObject(c.job, c.process.hProcess);
    for (const wchar_t* name : {L"GBARECOMP_WINDOW_HANDOFF", L"ROTK_SUPER_GAME_SHOW", L"ROTK_SUPER_GAME_READY",
                                L"ROTK_SUPER_GAME_BACK", L"ROTK_SUPER_GAME_SAVED", L"ROTK_SUPER_GAME_SHOW_FILE",
                                L"ROTK_SUPER_GAME_BACK_FILE", L"ROTK_SUPER_GAME_PID", L"ROTK_TTT_ROTK_SPRITES",
                                L"ROTK_TTT_ROTK_PORTRAITS", L"ROTK_TTT_ROTK_SOUNDS", L"ROTK_TTT_ROTK_HUD",
                                L"ROTK_TTT_ROTK_ROM"})
        SetEnvironmentVariableW(name, nullptr);
    if (hadModsRoot) SetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", modsRoot);
    SetEnvironmentVariableA("ROTK_PC_COLOR_VISION", hadColorVision ? colorVision : nullptr);
    if (!c.running) {
        ++c.failures;
        std::fprintf(stderr, "[rotk] super game: could not start %s\n", g_executable.string().c_str());
        return false;
    }
    std::fprintf(stderr, "[rotk] super game: The Two Towers started in the background\n");
    return true;
}

// Waits for `handle` (or `alsoEnds`), keeping the hidden window responsive. Its input belongs to The
// Two Towers meanwhile, so it is dropped.
void pumpWhile(HANDLE handle, DWORD timeoutMs, HANDLE alsoEnds)
{
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    HANDLE handles[2] = {handle, alsoEnds};
    while (timeoutMs == INFINITE || GetTickCount64() < until) {
        if (WaitForMultipleObjects(alsoEnds ? 2 : 1, handles, FALSE, kPumpMs) != WAIT_TIMEOUT) return;
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    }
}

// After the fade out: The Two Towers shows in this window's place, this one hides until it comes back.
void playTwoTowers()
{
    Companion& c = g_companion;
    if (!startCompanion()) return;
    SDL_Window* window = gameWindow();
    int w = 0, h = 0;
    if (window) SDL_GetWindowSize(window, &w, &h);
    {
        std::ofstream place(c.showFile, std::ios::trunc);
        place << windowPlace(window) << "\n";
    }
    std::error_code error;
    fs::remove(c.backFile, error);
    ResetEvent(c.ready);
    ResetEvent(c.back);
    AllowSetForegroundWindow(c.process.dwProcessId);
    SetEvent(c.show);
    std::fprintf(stderr, "[rotk] super game: to The Two Towers\n");

    // Muted while it plays. Only the devices this game had playing resume afterwards.
    bool paused[kMaxAudioDeviceId + 1] = {};
    for (int id = 1; id <= kMaxAudioDeviceId; ++id)
        if (SDL_GetAudioDeviceStatus(static_cast<SDL_AudioDeviceID>(id)) == SDL_AUDIO_PLAYING) {
            SDL_PauseAudioDevice(static_cast<SDL_AudioDeviceID>(id), 1);
            paused[id] = true;
        }
    // This window stays until The Two Towers shows (at once, or once it has booted), then gives way.
    pumpWhile(c.ready, kShowTimeoutMs, c.process.hProcess);
    if (window) SDL_HideWindow(window);
    // Back through its Return of the King row (this window shows where its was), or when it quits.
    pumpWhile(c.back, INFINITE, c.process.hProcess);
    if (window) {
        int bx = 0, by = 0, bw = 0, bh = 0, bfull = 0;
        std::ifstream place(c.backFile);
        std::string line;
        if (std::getline(place, line) && std::sscanf(line.c_str(), "%d,%d,%d,%d,%d", &bx, &by, &bw, &bh, &bfull) == 5) {
            if (bfull) {
                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else {
                SDL_SetWindowFullscreen(window, 0);
                // The Two Towers' window is the GBA's shape: this one gets its own shape back (wider
                // with widescreen), at that height and centered where that one was.
                if (bw > 0 && bh > 0 && w > 0 && h > 0) {
                    const int ownWidth = bh * w / h;
                    bx -= (ownWidth - bw) / 2;
                    bw = ownWidth;
                }
                if (bw > 0 && bh > 0) SDL_SetWindowSize(window, bw, bh);
                SDL_SetWindowPosition(window, bx, by);
            }
        }
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
    }
    // its heroes: re-read once it has written its save (SAVED), or now if it quit
    c.savePending = true;
    companionAlive();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    for (int id = 1; id <= kMaxAudioDeviceId; ++id)
        if (paused[id]) SDL_PauseAudioDevice(static_cast<SDL_AudioDeviceID>(id), 0);
    g_fadeIn = kFadeFrames;
    std::fprintf(stderr, "[rotk] super game: back from The Two Towers\n");
}

void darken(const gbarecomp::PresentFrame* frame, int level)  // 0 black .. kFadeFrames as is
{
    if (level >= kFadeFrames) return;
    if (level < 0) level = 0;
    for (unsigned y = 0; y < frame->height; ++y) {
        std::uint8_t* pixel = frame->rgb + std::size_t(y) * frame->stride * 3u;
        for (unsigned x = 0; x < frame->width * 3u; ++x)
            pixel[x] = static_cast<std::uint8_t>(pixel[x] * level / kFadeFrames);
    }
}
#endif

}  // namespace

void superGameRegister(gbarecomp::RunOptions& options)
{
    gba_mod_register_activation_plugin("rotk.pc.ttt-rotk-sprites", tttRotkSpritesActivated);
    gba_mod_register_activation_plugin("rotk.pc.ttt-rotk-portraits", tttRotkPortraitsActivated);
    gba_mod_register_activation_plugin("rotk.pc.ttt-rotk-sounds", tttRotkSoundsActivated);
    gba_mod_register_activation_plugin("rotk.pc.ttt-rotk-hud", tttRotkHudActivated);
    gba_mod_register_reset_callback(tttRotkSpritesReset);
    options.launcher_companion_label = "The Two Towers ROM";
    options.launcher_companion_note =
        "Your own copy of The Two Towers (USA, Europe) for the main menu's The Two Towers row and "
        "for importing its heroes. Without a pick, a copy beside this game's ROM is used.";
    options.launcher_companion_status = companionStatus;
    options.launcher_companion_chosen = companionChosen;
}

void superGameFind(const std::vector<std::string>& args)
{
    g_rom = twoTowersRomPath();
    g_executable = findExecutable();
    g_bios = findBios(args);
    if (g_rom.empty() || g_executable.empty()) return;
    // its own save beside its ROM, apart from the .sav an emulator keeps there
    g_save = g_rom.parent_path() / (g_rom.stem().string() + "-recompiled.eeprom");
    std::fprintf(stderr, "[rotk] super game: %s\n", g_executable.string().c_str());
}

bool superGameAvailable()
{
#if defined(_WIN32)
    return !g_rom.empty() && !g_executable.empty();
#else
    return false;
#endif
}

void superGamePoll(std::uint32_t superGameField)
{
    const std::uint32_t value = bus_read_u32(superGameField);
    if (value & kSwitch) bus_write_u32(superGameField, value & ~kSwitch);
#if defined(_WIN32)
    if (!superGameAvailable()) return;
    Companion& c = g_companion;
    // ready in the background from the start (and again if its window was closed)
    if (!companionAlive() && c.failures < 3) startCompanion();
    if (c.savePending && c.saved && WaitForSingleObject(c.saved, 0) == WAIT_OBJECT_0) {
        twoTowersReloadSave();
        c.savePending = false;
    }
    if (g_fadeIn > 0) --g_fadeIn;
    if ((value & kSwitch) && g_fadeOut < 0) g_fadeOut = kFadeFrames;
    if (g_fadeOut > 0) {
        --g_fadeOut;
    } else if (g_fadeOut == 0) {
        g_fadeOut = -1;
        playTwoTowers();
    }
#endif
}

void superGameDecorate(const gbarecomp::PresentFrame* frame)
{
#if defined(_WIN32)
    if (g_fadeOut >= 0) darken(frame, g_fadeOut);
    else if (g_fadeIn > 0) darken(frame, kFadeFrames - g_fadeIn);
#else
    (void)frame;
#endif
}

bool superGameDecorateActive()
{
#if defined(_WIN32)
    return g_fadeOut >= 0 || g_fadeIn > 0;
#else
    return false;
#endif
}

}  // namespace rotk
