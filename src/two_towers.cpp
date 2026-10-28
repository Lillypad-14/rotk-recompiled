#include "two_towers.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include "expanded_edition.h"
#include "runtime_arm.h"
#include "sha1.h"

namespace rotk {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kRomSize = 16u * 1024u * 1024u;
constexpr const char* kRomSha1 = "82b3b4afb0ed0d3fca646aa266e0bfc5bffd49d9";
constexpr std::uint32_t kRomBase = 0x08000000u;
constexpr std::uint32_t kMagic = 0x31445454u;  // "TTD1"
constexpr std::uint32_t kLanguagePackages = 0x0832EBA4u;
constexpr std::uint32_t kPrefixes = 0x080504F0u, kPrefixCount = 152;
constexpr std::uint32_t kBases = 0x08050E70u, kBaseCount = 127;
constexpr std::uint32_t kSuffixes = 0x08051660u, kSuffixCount = 58;
constexpr std::uint32_t kRecordSize = 16;
constexpr std::uint32_t kStringCount = 1610;
constexpr std::uint32_t kHuffmanRoot = 0x100;
// Two Towers' package for each ROTK language: EN, FR, DE, IT, ES (Two Towers adds Dutch as 1).
constexpr std::array<std::uint32_t, 5> kPackageForLanguage = {0, 2, 3, 4, 5};
constexpr std::uint32_t kHeaderSize = 0x20 + 4 * kPackageForLanguage.size();
// Strings carried besides the item names: hero names, stat names and the skill page text.
constexpr std::pair<std::uint32_t, std::uint32_t> kStringRanges[] = {{18, 23}, {271, 326}, {345, 414}};

std::vector<std::uint8_t> g_block;  // built with host RAM offsets from 0, relocated on write
std::vector<std::uint32_t> g_pointers;  // offsets in g_block holding block-relative addresses

std::vector<std::uint8_t> load(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool isTwoTowers(const fs::path& path)
{
    std::error_code error;
    if (fs::file_size(path, error) != kRomSize || error) return false;
    std::ifstream file(path, std::ios::binary);
    char header[0x10] = {};
    file.seekg(0xA0);
    file.read(header, sizeof header);
    return std::memcmp(header, "TWO TOWERS", 10) == 0 && std::memcmp(header + 0x0C, "ALPE", 4) == 0;
}

std::vector<std::uint8_t> findRom(const std::vector<std::string>& args)
{
    std::vector<fs::path> candidates;
    if (const char* path = std::getenv("ROTK_PC_TWO_TOWERS_ROM")) candidates.emplace_back(path);
    std::vector<fs::path> folders = {executableFolder()};
    if (const std::string rom = sessionRomPath(args); !rom.empty())
        folders.insert(folders.begin(), fs::absolute(rom).parent_path());
    for (const fs::path& folder : folders) {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder, error))
            if (entry.path().extension() == ".gba" && isTwoTowers(entry.path())) candidates.push_back(entry.path());
    }
    for (const fs::path& path : candidates) {
        std::vector<std::uint8_t> rom = load(path);
        if (rom.size() == kRomSize && gba::sha1(rom.data(), rom.size()).hex() == kRomSha1) {
            std::fprintf(stderr, "[rotk] The Two Towers: %s\n", path.string().c_str());
            return rom;
        }
    }
    return {};
}

std::uint32_t read32(const std::vector<std::uint8_t>& rom, std::uint32_t address)
{
    std::uint32_t value;
    std::memcpy(&value, rom.data() + (address - kRomBase), 4);
    return value;
}

std::uint16_t read16(const std::vector<std::uint8_t>& rom, std::uint32_t address)
{
    std::uint16_t value;
    std::memcpy(&value, rom.data() + (address - kRomBase), 2);
    return value;
}

// One string of a Huffman text package: root node 0x100, node n at package + 4 + (n - 0x100) * 4
// holding {u16 bit-0 child, u16 bit-1 child}, a value below 0x100 is a leaf, bits LSB-first,
// NUL ends the string.
std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& rom, std::uint32_t package, std::uint32_t id)
{
    const std::uint32_t offsets = package + read32(rom, package);
    std::uint32_t stream = package + read32(rom, offsets + id * 4);
    std::vector<std::uint8_t> out;
    std::uint8_t byte = rom[stream++ - kRomBase];
    std::uint32_t bit = 0;
    while (out.size() < 0x400) {
        std::uint32_t node = kHuffmanRoot;
        while (node >= kHuffmanRoot) {
            node = read16(rom, package + 4 + (node - kHuffmanRoot) * 4 + ((byte >> bit) & 1 ? 2 : 0));
            if (++bit == 8) bit = 0, byte = rom[stream++ - kRomBase];
        }
        if (node == 0) break;
        out.push_back(static_cast<std::uint8_t>(node));
    }
    out.push_back(0);
    return out;
}

void put32(std::uint32_t offset, std::uint32_t value)
{
    std::memcpy(g_block.data() + offset, &value, 4);
}

std::uint32_t append(const std::uint8_t* data, std::size_t size)
{
    const std::uint32_t offset = static_cast<std::uint32_t>(g_block.size());
    g_block.insert(g_block.end(), data, data + size);
    g_block.resize((g_block.size() + 3) & ~std::size_t{3});
    return offset;
}

void putPointer(std::uint32_t offset, std::uint32_t target)
{
    put32(offset, target);
    g_pointers.push_back(offset);
}

void build(const std::vector<std::uint8_t>& rom)
{
    g_block.assign(kHeaderSize, 0);
    put32(0x00, kMagic);
    std::set<std::uint32_t> ids;
    const std::pair<std::uint32_t, std::uint32_t> tables[] = {
        {kPrefixes, kPrefixCount}, {kBases, kBaseCount}, {kSuffixes, kSuffixCount}};
    for (std::size_t t = 0; t < 3; ++t) {
        const auto [address, count] = tables[t];
        putPointer(0x04 + t * 8, append(rom.data() + (address - kRomBase), count * kRecordSize));
        put32(0x08 + t * 8, count);
        for (std::uint32_t i = 0; i < count; ++i) ids.insert(read16(rom, address + i * kRecordSize));
    }
    for (const auto& [first, last] : kStringRanges)
        for (std::uint32_t id = first; id <= last; ++id) ids.insert(id);
    put32(0x1C, kStringCount);
    for (std::size_t language = 0; language < kPackageForLanguage.size(); ++language) {
        const std::uint32_t package = read32(rom, kLanguagePackages + kPackageForLanguage[language] * 4);
        std::vector<std::uint32_t> table(kStringCount, 0);
        std::vector<std::uint32_t> placed(kStringCount, 0);
        for (std::uint32_t id : ids) {
            if (id >= kStringCount) continue;
            const std::vector<std::uint8_t> text = decode(rom, package, id);
            placed[id] = append(text.data(), text.size());
        }
        const std::uint32_t tableOffset =
            append(reinterpret_cast<const std::uint8_t*>(table.data()), table.size() * 4);
        for (std::uint32_t id = 0; id < kStringCount; ++id)
            if (placed[id]) putPointer(tableOffset + id * 4, placed[id]);
        putPointer(0x20 + static_cast<std::uint32_t>(language) * 4, tableOffset);
    }
}

}  // namespace

void twoTowersFind(const std::vector<std::string>& args)
{
    const std::vector<std::uint8_t> rom = findRom(args);
    if (rom.empty()) return;
    build(rom);
    std::fprintf(stderr, "[rotk] The Two Towers data: %zu KiB\n", g_block.size() / 1024);
}

std::uint32_t twoTowersBlockSize()
{
    return static_cast<std::uint32_t>(g_block.size());
}

std::vector<std::uint8_t> twoTowersBlockAt(std::uint32_t address)
{
    std::vector<std::uint8_t> block = g_block;
    for (std::uint32_t offset : g_pointers) {
        std::uint32_t value;
        std::memcpy(&value, block.data() + offset, 4);
        value += address;
        std::memcpy(block.data() + offset, &value, 4);
    }
    return block;
}

void twoTowersWrite(std::uint32_t address)
{
    const std::vector<std::uint8_t> block = twoTowersBlockAt(address);
    for (std::size_t i = 0; i < block.size(); i += 4) {
        std::uint32_t word;
        std::memcpy(&word, block.data() + i, 4);
        bus_write_u32(address + static_cast<std::uint32_t>(i), word);
    }
}

}  // namespace rotk
