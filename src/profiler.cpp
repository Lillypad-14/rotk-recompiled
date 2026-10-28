#include "profiler.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#pragma comment(lib, "winmm")
#endif

namespace rotk {
namespace {

// Test aid: a sampling profiler of the emulation thread. While sampling is on, a second thread
// suspends the emulation thread about every 250 us, reads its instruction pointer and counts it
// by offset into the executable. tools/profile_report.py names the offsets with the executable's
// symbols. Windows only.

std::atomic<bool> g_on{false};
std::atomic<bool> g_quit{false};
std::unordered_map<std::uint64_t, std::uint64_t> g_counts;  // image offset -> samples, sampler thread only
// Samples in another module: (address there, offset of the first executable frame below it).
std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, std::uint64_t>> g_others;
std::thread g_thread;
const char* g_path = nullptr;

#if defined(_WIN32)
HANDLE g_target = nullptr;

void sampler()
{
    const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(GetModuleHandleW(nullptr), &info, sizeof(info));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const std::uint64_t size = nt->OptionalHeader.SizeOfImage;
    timeBeginPeriod(1);
    while (!g_quit.load()) {
        if (g_on.load() && SuspendThread(g_target) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(g_target, &context)) {
                const std::uint64_t rip = context.Rip;
                if (rip >= base && rip < base + size) {
                    ++g_counts[rip - base];
                } else {
                    // Unwind (x64 unwind data) to the executable's frame that led here.
                    std::uint64_t caller = 0;
                    for (int depth = 0; depth < 48 && context.Rip; ++depth) {
                        if (context.Rip >= base && context.Rip < base + size) {
                            caller = context.Rip - base;
                            break;
                        }
                        DWORD64 imageBase = 0;
                        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                        if (!entry) {
                            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                            context.Rsp += 8;
                            continue;
                        }
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData,
                                         &establisher, nullptr);
                    }
                    ++g_others[rip][caller];
                }
            }
            ResumeThread(g_target);
        }
        // Sleeps are about a millisecond on Windows, so the interval is spun.
        const auto next = std::chrono::steady_clock::now() + std::chrono::microseconds(250);
        while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
    }
    timeEndPeriod(1);
}
#endif

#if defined(_WIN32)
// "<module>!<nearest export below the address>" for an address outside the executable.
std::string otherName(std::uint64_t address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return "unknown";
    char path[MAX_PATH] = "";
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string name = path;
    name = name.substr(name.find_last_of("\\/") + 1);
    const auto base = reinterpret_cast<std::uint64_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return name;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    std::uint64_t best = 0;
    const char* bestName = nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const std::uint64_t at = base + functions[ordinals[i]];
        if (at <= address && at > best) {
            best = at;
            bestName = reinterpret_cast<const char*>(base + names[i]);
        }
    }
    return bestName ? name + "!" + bestName : name;
}
#endif

void write()
{
    g_quit = true;
    if (g_thread.joinable()) g_thread.join();
    std::FILE* file = std::fopen(g_path, "w");
    if (!file) return;
    std::fprintf(file, "# offset samples, then module <name> samples for other modules\n");
#if defined(_WIN32)
    std::unordered_map<std::string, std::unordered_map<std::uint64_t, std::uint64_t>> named;
    for (const auto& [address, callers] : g_others)
        for (const auto& [caller, count] : callers) named[otherName(address)][caller] += count;
    for (const auto& [name, callers] : named)
        for (const auto& [caller, count] : callers)
            std::fprintf(file, "module %s %llx %llu\n", name.c_str(), static_cast<unsigned long long>(caller),
                         static_cast<unsigned long long>(count));
#endif
    for (const auto& [offset, count] : g_counts)
        std::fprintf(file, "%llx %llu\n", static_cast<unsigned long long>(offset), static_cast<unsigned long long>(count));
    std::fclose(file);
}

}  // namespace

void profilerStart(const char* path)
{
#if defined(_WIN32)
    if (g_path || !path) return;
    g_path = path;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    g_thread = std::thread(sampler);
    std::atexit(write);
#else
    (void)path;
#endif
}

void profilerSample(bool on)
{
    g_on = on;
}

}  // namespace rotk
