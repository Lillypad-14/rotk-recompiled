#include "sample_packs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rotk {
namespace {

namespace fs = std::filesystem;

// A pack is a folder with manifest.txt, one sample per line: the ROM sample's start address in hex,
// its length in bytes, and a WAV file (mono or stereo, 8-bit, 16-bit or 32-bit float PCM). The WAV
// replaces the whole sample at any number of frames per original byte (the ratio), and its
// loop is the original's loop scaled by that ratio. The WAV's own sample rate is ignored: the game's
// pitch steps through it.

std::unordered_map<std::uint32_t, SampleReplacement> g_replacements;  // by start address
std::unordered_map<std::uint32_t, std::uint32_t> g_lengths;           // the original's length

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

std::uint32_t le(const std::string& b, std::size_t at, int bytes)
{
    std::uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + i])) << (8 * i);
    return v;
}

// Decodes a WAV to mono frames in s8 units.
bool readWav(const fs::path& path, std::vector<float>& out)
{
    std::ifstream file(path, std::ios::binary);
    const std::string b((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || b.compare(0, 4, "RIFF") || b.compare(8, 4, "WAVE")) return false;
    std::uint32_t format = 0, channels = 0, bits = 0;
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const std::uint32_t size = le(b, at + 4, 4);
        if (at + 8 + size > b.size()) return false;
        if (!b.compare(at, 4, "fmt ") && size >= 16) {
            format = le(b, at + 8, 2);
            channels = le(b, at + 10, 2);
            bits = le(b, at + 22, 2);
        } else if (!b.compare(at, 4, "data") && channels) {
            const std::uint32_t frameBytes = channels * bits / 8;
            if (!frameBytes || !((format == 1 && (bits == 8 || bits == 16)) || (format == 3 && bits == 32)))
                return false;
            for (std::size_t f = 0; f + frameBytes <= size; f += frameBytes) {
                float sum = 0.0f;
                for (std::uint32_t c = 0; c < channels; ++c) {
                    const std::size_t p = at + 8 + f + c * (bits / 8);
                    if (bits == 8) sum += static_cast<float>(static_cast<std::uint8_t>(b[p])) - 128.0f;
                    else if (bits == 16) sum += static_cast<std::int16_t>(le(b, p, 2)) / 256.0f;
                    else {
                        const std::uint32_t raw = le(b, p, 4);
                        float value;
                        std::memcpy(&value, &raw, 4);
                        sum += value * 128.0f;
                    }
                }
                out.push_back(sum / static_cast<float>(channels));
            }
            return !out.empty();
        }
        at += 8 + size + (size & 1u);
    }
    return false;
}

void loadPack(const fs::path& folder)
{
    std::ifstream manifest(folder / "manifest.txt");
    std::string line;
    int loaded = 0;
    while (std::getline(manifest, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string address, file;
        std::uint32_t length = 0;
        if (!(fields >> address >> length >> file) || !length) continue;
        const std::uint32_t start = static_cast<std::uint32_t>(std::strtoul(address.c_str(), nullptr, 16));
        SampleReplacement replacement;
        if (!readWav(folder / file, replacement.frames)) {
            std::fprintf(stderr, "[rotk] sample pack %s: cannot read %s\n", folder.filename().string().c_str(),
                         file.c_str());
            continue;
        }
        replacement.ratio = static_cast<double>(replacement.frames.size()) / length;
        g_replacements[start] = std::move(replacement);
        g_lengths[start] = length;
        ++loaded;
    }
    std::fprintf(stderr, "[rotk] sample pack %s: %d samples\n", folder.filename().string().c_str(), loaded);
}

}  // namespace

void samplePacksClear()
{
    g_replacements.clear();
    g_lengths.clear();
}

void samplePacksLoad()
{
    samplePacksClear();
    std::error_code error;
    const fs::path packages = executableDirectory() / "mods" / "packages" / "rotk.pc";
    for (const auto& version : fs::directory_iterator(packages, error))
        for (const auto& pack : fs::directory_iterator(version.path() / "samples", error))
            if (pack.is_directory()) loadPack(pack.path());
}

const SampleReplacement* samplePackFind(const SampleSpan& span)
{
    if (g_replacements.empty()) return nullptr;
    const auto found = g_replacements.find(span.start);
    if (found == g_replacements.end() || g_lengths[span.start] != span.end - span.start) return nullptr;
    return &found->second;
}

void samplePackNote(const SampleSpan& span, std::uint8_t (*readByte)(std::uint32_t))
{
    static const char* folder = std::getenv("ROTK_PC_SAMPLE_DUMP");
    static std::unordered_set<std::uint32_t> written;
    if (!folder || span.end <= span.start || span.end - span.start > (1u << 24) || !written.insert(span.start).second)
        return;
    std::error_code error;
    fs::create_directories(folder, error);
    char name[32];
    std::snprintf(name, sizeof(name), "%08x.wav", span.start);
    const std::uint32_t length = span.end - span.start;
    std::ofstream wav(fs::path(folder) / name, std::ios::binary);
    const auto put = [&](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) wav.put(static_cast<char>(v >> (8 * i)));
    };
    wav.write("RIFF", 4);
    put(36 + length, 4);
    wav.write("WAVEfmt ", 8);
    put(16, 4);
    put(1, 2);
    put(1, 2);
    put(16384, 4);  // nominal: the game's pitch decides the real rate
    put(16384, 4);
    put(1, 2);
    put(8, 2);
    wav.write("data", 4);
    put(length, 4);
    for (std::uint32_t a = span.start; a < span.end; ++a) wav.put(static_cast<char>(readByte(a)));
    std::ofstream list(fs::path(folder) / "samples.txt", std::ios::app);
    list << std::hex << span.start << std::dec << " " << length << " " << name << "  # loop mode "
         << int(span.loopMode) << ", loop " << span.loopLen << " bytes\n";
}

}  // namespace rotk
