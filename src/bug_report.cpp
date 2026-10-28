#include "bug_report.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#include <shellapi.h>
#pragma comment(lib, "shell32")
#else
#include <unistd.h>
#endif

namespace rotk {
namespace {

namespace fs = std::filesystem;

// The BugReport hotkey (F12): copies what a developer needs to replay the session into
// bug-reports/<time>/ beside the executable and opens that folder. Diagnostic replays
// (diagnostics/replays, recorded by the framework in windowed play) and co-op recordings
// (<save>.coop-<time>.*) both replay exactly; see docs/BUG_REPORTS.md.

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

int processId()
{
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

bool copyInto(const fs::path& file, const fs::path& folder, std::vector<std::string>& copied)
{
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return false;
    fs::copy_file(file, folder / file.filename(), fs::copy_options::overwrite_existing, error);
    if (error) return false;
    copied.push_back(file.filename().string());
    return true;
}

// The newest co-op recording beside the save: <save>.coop-<time>-1.sram, -2.sram and .inputs.txt.
std::vector<fs::path> newestCoopRecording(const fs::path& save)
{
    std::error_code error;
    const std::string prefix = save.filename().string() + ".coop-";
    std::string newest;
    for (const auto& entry : fs::directory_iterator(save.parent_path(), error)) {
        const std::string name = entry.path().filename().string();
        const std::string suffix = ".inputs.txt";
        if (name.rfind(prefix, 0) == 0 && name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            newest = std::max(newest, name.substr(0, name.size() - suffix.size()));
    }
    if (newest.empty()) return {};
    const fs::path base = save.parent_path() / newest;
    return {fs::path(base.string() + ".inputs.txt"), fs::path(base.string() + "-1.sram"),
            fs::path(base.string() + "-2.sram")};
}

void bugReport(const char* savePath)
{
    const fs::path exe = executableDirectory();
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    const fs::path folder = exe / "bug-reports" / stamp;
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) {
        std::fprintf(stderr, "[rotk] bug report: cannot create %s\n", folder.string().c_str());
        return;
    }
    std::vector<std::string> copied;
    // This session's diagnostic replay segments and their sidecars.
    const std::string session = "-" + std::to_string(processId()) + "-";
    for (const auto& entry : fs::directory_iterator(exe / "diagnostics" / "replays", error))
        if (entry.path().filename().string().find(session) != std::string::npos)
            copyInto(entry.path(), folder, copied);
    if (savePath && *savePath) {
        const fs::path save(savePath);
        copyInto(save, folder, copied);
        copyInto(save.parent_path() / (save.stem().string() + ".player2" + save.extension().string()), folder,
                 copied);
        for (const fs::path& file : newestCoopRecording(save)) copyInto(file, folder, copied);
    }
    copyInto(exe / "mods" / "state.toml", folder, copied);
    for (const auto& entry : fs::directory_iterator(fs::current_path(error), error))
        if (entry.path().extension() == ".ini") copyInto(entry.path(), folder, copied);

    std::ofstream readme(folder / "README.txt");
    readme << "ROTK Recompiled bug report, " << stamp << ".\n"
           << "Zip this folder and send it with a note of what went wrong and roughly when.\n"
           << "It holds this session's replay, your saves and settings, never the ROM.\n\nFiles:\n";
    for (const std::string& name : copied) readme << "  " << name << "\n";
    readme.close();
    std::fprintf(stderr, "[rotk] bug report: %zu files in %s\n", copied.size(), folder.string().c_str());
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

}  // namespace

void bugReportRegister(gbarecomp::RunOptions& options)
{
    options.bug_report = bugReport;
}

}  // namespace rotk
