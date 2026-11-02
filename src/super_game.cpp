#include "super_game.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "color_vision.h"
#include "expanded_edition.h"
#include "runtime_arm.h"
#include "two_towers.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#include <SDL.h>

namespace rotk {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kSwitch = 1u << 1;  // PC_HOST_SUPER_GAME_SWITCH
constexpr const char* kExecutable = "TTT-Recompiled.exe";
// How long this window waits for The Two Towers' first frame before giving way anyway.
constexpr unsigned kReadyTimeoutMs = 20000;
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

// Plays The Two Towers in this window's place until it quits.
void playTwoTowers()
{
    SDL_Window* window = gameWindow();
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = 0, h = 0;
    bool fullscreen = false;
    if (window) {
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    }
    const std::wstring readyName = L"Local\\RotkSuperGameReady" + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    // Set by its main menu's Return of the King row, its window's place written to backFile first.
    const std::wstring backName = L"Local\\RotkSuperGameBack" + std::to_wstring(GetCurrentProcessId());
    HANDLE back = CreateEventW(nullptr, TRUE, FALSE, backName.c_str());
    const fs::path backFile = fs::temp_directory_path() / ("rotk-super-game-" + std::to_string(GetCurrentProcessId()) + ".txt");
    std::error_code error;
    fs::remove(backFile, error);
    char geometry[96];
    std::snprintf(geometry, sizeof geometry, "%d,%d,%d,%d,%d", x, y, w, h, fullscreen ? 1 : 0);
    SetEnvironmentVariableA("GBARECOMP_WINDOW_HANDOFF", geometry);
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_READY", readyName.c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_BACK", backName.c_str());
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_BACK_FILE", backFile.wstring().c_str());
    // ROTK's mod selection (GBARECOMP_MODS_ROOT, set by the launcher) is ROTK's code: The Two Towers
    // keeps its own. CPU boost and stereo carry over through their own variables.
    wchar_t modsRoot[MAX_PATH] = {};
    const bool hadModsRoot = GetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", modsRoot, MAX_PATH) != 0;
    SetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", nullptr);
    // Display features the plain host has carry over as ROTK has them set now.
    char colorVision[16] = {};
    const bool hadColorVision = GetEnvironmentVariableA("ROTK_PC_COLOR_VISION", colorVision, sizeof colorVision) != 0;
    SetEnvironmentVariableA("ROTK_PC_COLOR_VISION", colorVisionModeName());

    // straight into the game: the GBA boot logo would break the swap
    std::string command = quoted(g_executable.string()) + " --window --bios-skip-intro --rom " + quoted(g_rom.string()) +
                          " --save-path " + quoted(g_save.string());
    if (!g_bios.empty()) command += " --bios " + quoted(g_bios.string());
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // It runs in this game's folder, so it shares the control bindings (keybinds.ini, input.ini).
    const bool started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        nullptr, &startup, &process) != 0;
    SetEnvironmentVariableA("GBARECOMP_WINDOW_HANDOFF", nullptr);
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_READY", nullptr);
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_BACK", nullptr);
    SetEnvironmentVariableW(L"ROTK_SUPER_GAME_BACK_FILE", nullptr);
    if (hadModsRoot) SetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", modsRoot);
    SetEnvironmentVariableA("ROTK_PC_COLOR_VISION", hadColorVision ? colorVision : nullptr);
    if (!started) {
        std::fprintf(stderr, "[rotk] super game: could not start %s\n", g_executable.string().c_str());
        if (ready) CloseHandle(ready);
        if (back) CloseHandle(back);
        return;
    }
    std::fprintf(stderr, "[rotk] super game: The Two Towers started\n");

    // Muted while it plays. Only the devices this game had playing resume afterwards.
    bool paused[kMaxAudioDeviceId + 1] = {};
    for (int id = 1; id <= kMaxAudioDeviceId; ++id)
        if (SDL_GetAudioDeviceStatus(static_cast<SDL_AudioDeviceID>(id)) == SDL_AUDIO_PLAYING) {
            SDL_PauseAudioDevice(static_cast<SDL_AudioDeviceID>(id), 1);
            paused[id] = true;
        }
    // This window stays until The Two Towers has drawn into its own, then gives way.
    if (ready) pumpWhile(ready, kReadyTimeoutMs, process.hProcess);
    if (window) SDL_HideWindow(window);
    // Back through its Return of the King row (this window shows where its was, before it closes),
    // or when it quits some other way.
    if (back) pumpWhile(back, INFINITE, process.hProcess);
    if (window) {
        int bx = 0, by = 0, bw = 0, bh = 0, bfull = 0;
        std::ifstream place(backFile);
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
    pumpWhile(process.hProcess, INFINITE, nullptr);
    fs::remove(backFile, error);
    // its heroes, saved just now, can be imported at once
    twoTowersReloadSave();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    for (int id = 1; id <= kMaxAudioDeviceId; ++id)
        if (paused[id]) SDL_PauseAudioDevice(static_cast<SDL_AudioDeviceID>(id), 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (ready) CloseHandle(ready);
    if (back) CloseHandle(back);
    std::fprintf(stderr, "[rotk] super game: back from The Two Towers\n");
}
#endif

}  // namespace

void superGameRegister(gbarecomp::RunOptions& options)
{
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
    if (!(value & kSwitch)) return;
    bus_write_u32(superGameField, value & ~kSwitch);
#if defined(_WIN32)
    if (superGameAvailable()) playTwoTowers();
#endif
}

}  // namespace rotk
