#include "expanded_edition.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bps_patch.h"
#include "sha1.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#include <SDL.h>

namespace rotk {
namespace {

namespace fs = std::filesystem;

// The launcher's "Expanded" row (front page). The vanilla build carries the Expanded mod as a BPS
// patch (ROTK_EDITION_PATCH, beside the executable) and hands the session to the Expanded build
// (ROTK_EDITION_EXE): the patched ROM is made once beside the player's ROM, checked against
// ROTK_EDITION_SHA1, and reused while it still matches. The original ROM is never changed.

bool g_chosen = false;

void chosen(int enabled)
{
    g_chosen = enabled != 0;
}

std::vector<std::uint8_t> load(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

fs::path executableDirectory()
{
#if defined(_WIN32)
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return {};
    return fs::path(path).parent_path();
#else
    std::error_code error;
    return fs::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

fs::path besideExecutable(const char* path)
{
    const fs::path given(path);
    return given.is_absolute() ? given : executableDirectory() / given;
}

std::string argValue(const std::vector<std::string>& args, const char* name)
{
    for (std::size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return {};
}

// The ROM the session would boot: --rom, else [rom] path in the --config file.
std::string sourceRom(const std::vector<std::string>& args)
{
    if (std::string rom = argValue(args, "--rom"); !rom.empty()) return rom;
    std::ifstream config(argValue(args, "--config"));
    std::string line, section;
    while (std::getline(config, line)) {
        if (!line.empty() && line[0] == '[') section = line;
        const auto quote = line.find('"');
        if (section == "[rom]" && line.rfind("path", 0) == 0 && quote != std::string::npos)
            return line.substr(quote + 1, line.find('"', quote + 1) - quote - 1);
    }
    return {};
}

std::string quoted(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (char c : arg) out += c == '"' ? std::string("\\\"") : std::string(1, c);
    return out + "\"";
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "[rotk] Expanded: %s\n", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ROTK Expanded", message.c_str(), nullptr);
    return 1;
}

// The Expanded build remembers the ROM it played (the Expanded copy) as the last pick, in the rom.cfg
// both builds share beside the executables, so the launcher would open on a ROM it doesn't recognize
// as the original. Point the pick back at the original beside the copy.
void restoreOriginalRomPick()
{
    const fs::path cache = executableDirectory() / "rom.cfg";
    std::string line;
    {
        std::ifstream in(cache);
        std::getline(in, line);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const std::string suffix = "-expanded";
    const fs::path rom(line);
    const std::string stem = rom.stem().string();
    if (stem.size() <= suffix.size() || stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) != 0) return;
    const fs::path original = rom.parent_path() / (stem.substr(0, stem.size() - suffix.size()) + rom.extension().string());
    std::error_code error;
    if (!fs::is_regular_file(original, error)) return;
    std::ofstream(cache, std::ios::trunc) << original.string() << "\n";
    std::fprintf(stderr, "[rotk] Expanded: the launcher's ROM is the original again (%s)\n", original.string().c_str());
}

}  // namespace

std::filesystem::path executableFolder()
{
    return executableDirectory();
}

std::string sessionRomPath(const std::vector<std::string>& args)
{
    return sourceRom(args);
}

void expandedEditionRegister(gbarecomp::RunOptions& options)
{
#if defined(ROTK_EDITION_PATCH)
    options.launcher_edition_label = "Expanded";
    options.launcher_edition_note =
        "Play the ROTK Expanded mod: new skills, heroes, co-op map pages and more. Your ROM is "
        "patched into a copy beside it the first time, and Expanded keeps its own save.";
    options.launcher_edition_chosen = chosen;
    options.launcher_edition_sha1 = ROTK_EDITION_SHA1;
    restoreOriginalRomPick();
#else
    (void)options;
#endif
}

int expandedEditionDispatch(const std::vector<std::string>& args)
{
#if defined(ROTK_EDITION_PATCH)
    const fs::path rom = sourceRom(args);
    std::error_code error;
    if (rom.empty() || !fs::is_regular_file(rom, error)) return -1;  // the runtime reports it
    const auto source = load(rom);
    const bool alreadyExpanded = gba::sha1(source.data(), source.size()).hex() == ROTK_EDITION_SHA1;
    if (!g_chosen) {
        if (alreadyExpanded)
            return fail("This ROM is already the Expanded mod. Turn Expanded on to play it, or pick "
                        "your unmodified ROM for the original game.");
        return -1;
    }

    // The Expanded ROM: the chosen ROM when it already is one, else a copy made beside it (or in
    // the game folder when that folder can't be written), reused while it matches this version.
    fs::path expanded = rom;
    if (!alreadyExpanded) {
        const std::string name = rom.stem().string() + "-expanded" + rom.extension().string();
        const fs::path candidates[] = {rom.parent_path() / name, executableDirectory() / "mods" / "editions" / name};
        expanded.clear();
        for (const fs::path& candidate : candidates) {
            const auto current = fs::is_regular_file(candidate, error) ? load(candidate) : std::vector<std::uint8_t>{};
            if (!current.empty() && gba::sha1(current.data(), current.size()).hex() == ROTK_EDITION_SHA1) {
                expanded = candidate;
                break;
            }
        }
        if (expanded.empty()) {
            const auto patch = load(besideExecutable(ROTK_EDITION_PATCH));
            if (patch.empty()) return fail("The Expanded patch (expanded.bps) is missing from the game folder.");
            std::vector<std::uint8_t> target;
            std::string why;
            if (!bpsApply(source, patch, target, &why))
                return fail("Could not make the Expanded ROM: " + why +
                            ". Pick your unmodified ROM of the game (an older Expanded ROM can't be updated).");
            if (gba::sha1(target.data(), target.size()).hex() != ROTK_EDITION_SHA1)
                return fail("This Expanded patch does not match this version of the game.");
            for (const fs::path& candidate : candidates) {
                // Written whole to a temporary file first, so a half-written ROM is never reused.
                fs::create_directories(candidate.parent_path(), error);
                const fs::path temporary = candidate.string() + ".tmp";
                {
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(target.data()), static_cast<std::streamsize>(target.size()));
                    if (!file) continue;
                }
                fs::rename(temporary, candidate, error);
                if (error) {
                    fs::remove(temporary, error);
                    continue;
                }
                expanded = candidate;
                break;
            }
            if (expanded.empty()) return fail("Could not save the Expanded ROM beside your ROM or in the game folder.");
            std::fprintf(stderr, "[rotk] Expanded: made %s\n", expanded.string().c_str());
        }
    }

    // The same session arguments, with the Expanded ROM and its own save beside the normal one.
    std::vector<std::string> forwarded = {besideExecutable(ROTK_EDITION_EXE).string(), "--no-launcher", "--rom",
                                          expanded.string()};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--rom" || arg == "--rom-sha1" || arg == "--rom-crc32" || arg == "--config" ||
            arg == "--state-dir") {
            ++i;
        } else if (arg == "--save-path" && i + 1 < args.size()) {
            const fs::path save(args[++i]);
            forwarded.push_back("--save-path");
            forwarded.push_back((save.parent_path() / (save.stem().string() + "-expanded" + save.extension().string())).string());
        } else if (arg != "--launcher" && arg != "--no-launcher") {
            forwarded.push_back(arg);
        }
    }
#if defined(_WIN32)
    if (!fs::is_regular_file(forwarded[0], error))
        return fail("The Expanded game (" + forwarded[0] + ") is missing from the game folder.");
    // It plays with the mod settings chosen in this launcher.
    SetEnvironmentVariableW(L"GBARECOMP_MODS_ROOT", (executableDirectory() / "mods").wstring().c_str());
    std::string command;
    for (const std::string& arg : forwarded) command += (command.empty() ? "" : " ") + quoted(arg);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return fail("Could not start the Expanded game (" + forwarded[0] + ").");
    // Stay until it ends, so whatever started this game (a script, a shortcut) sees it finish.
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    return fail("Expanded is only set up on Windows for now.");
#endif
#else
    (void)args;
    return -1;
#endif
}

}  // namespace rotk
