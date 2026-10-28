#include "two_towers.h"

#include <algorithm>
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
constexpr std::uint32_t kSaveSlotsField = 0x20 + 4 * kPackageForLanguage.size();
constexpr std::uint32_t kIconsField = kSaveSlotsField + 8;
constexpr std::uint32_t kSkillArtField = kIconsField + 4;
constexpr std::uint32_t kSpritesField = kSkillArtField + 4;
constexpr std::uint32_t kHeaderSize = kSpritesField + 8;
// Backpack icons: a 16-byte SpriteAnimation per base item (tiles, frame set, palette pointers and
// a frame duration). Frame 0 is the 24x24 backpack icon: a gold bevel around a dark green fill.
// A Two Towers frame is {u8 flags (OAM piece count in bits 0-4), u8, u8 width, u8 height,
// u16 tile stream offset}, then per piece {s16 x, s16 y, u8 size, u8 shape, u8 width, u8 height,
// u32 tile byte offset}.
constexpr std::uint32_t kBaseIcons = 0x080519F0u;
constexpr std::uint32_t kIconRecordSize = 16;
constexpr std::uint32_t kFrameSetHeaderSize = 12;
constexpr std::uint32_t kFramePieceSize = 12;
constexpr int kTwoTowersIcon = 24;
// ROTK's backpack icon: 32x24, one frame of a 32x8 and a 32x16 OBJ, a white frame 29x23 with a
// black line inside its top and left edges, and a transparent background.
constexpr int kIconWidth = 32, kIconHeight = 24;
constexpr int kFrameRight = 28, kFrameBottom = 22;
constexpr int kIconShiftX = 3;  // centers the Two Towers art inside the ROTK frame
constexpr std::uint16_t kWhite = 0x7FFF, kBlack = 0x0000;
// The Two Towers save: a 512-byte EEPROM image with each 8-byte block byte-reversed in the .sav
// file, and four 0x78-byte character slots at blocks 4, 19, 34 and 49 that sum to 0 as halfwords.
constexpr std::uint32_t kSaveSize = 512;
constexpr std::uint32_t kSaveBlock = 8;
constexpr std::uint32_t kSaveSlotSize = 0x78;
constexpr std::uint32_t kSaveSlotBlocks[] = {4, 19, 34, 49};
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

fs::path g_romPath;

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
            g_romPath = path;
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

// A Two Towers sprite tile stream. The commands match ROTK's decompress_lz77 but carry no length
// bias: 00 n:6 literal n bytes, 01 n:6 byte run of n, 1 d:7 d:3 n:5 copy n bytes from d back, with
// n = 0 a run of d + 64 of the next byte. A copy from before the start reads zeros.
std::vector<std::uint8_t> decompressTiles(const std::vector<std::uint8_t>& rom, std::uint32_t address)
{
    std::vector<std::uint8_t> out;
    std::uint32_t p = address - kRomBase;
    while (p < rom.size() && out.size() < 0x4000) {
        const std::uint8_t command = rom[p++];
        if (command == 0) break;
        if (command < 0x40) {
            out.insert(out.end(), rom.begin() + p, rom.begin() + p + command);
            p += command;
        } else if (command < 0x80) {
            out.insert(out.end(), command & 0x3F, rom[p++]);
        } else {
            const std::uint8_t operand = rom[p++];
            const std::uint32_t distance = (command & 0x7F) << 3 | operand >> 5, count = operand & 0x1F;
            if (count == 0) {
                out.insert(out.end(), distance + 64, rom[p++]);
            } else {
                for (std::uint32_t i = 0; i < count; ++i)
                    out.push_back(distance && distance <= out.size() ? out[out.size() - distance] : 0);
            }
        }
    }
    return out;
}

// A base item's Two Towers backpack icon redrawn as ROTK's: frame set, 4bpp tiles and palette,
// or empty when the record has none.
struct Icon {
    std::vector<std::uint8_t> frames, tiles, palette;
};

Icon convertIcon(const std::vector<std::uint8_t>& rom, std::uint32_t record)
{
    const std::uint32_t tileStream = read32(rom, record), frameSet = read32(rom, record + 4);
    const std::uint32_t paletteAddress = read32(rom, record + 8);
    auto inRom = [](std::uint32_t address) { return address >= kRomBase && address < kRomBase + kRomSize - 0x100; };
    if (!inRom(tileStream) || !inRom(frameSet) || !inRom(paletteAddress)) return {};
    const std::uint32_t frame = frameSet + kFrameSetHeaderSize + read16(rom, frameSet + kFrameSetHeaderSize);
    const std::vector<std::uint8_t> source = decompressTiles(rom, tileStream + read16(rom, frame + 4));

    std::uint8_t art[kTwoTowersIcon][kTwoTowersIcon] = {};
    for (std::uint32_t k = 0; k < (rom[frame - kRomBase] & 0x1Fu); ++k) {
        const std::uint32_t piece = frame + 6 + k * kFramePieceSize;
        const int x = static_cast<std::int16_t>(read16(rom, piece)), y = static_cast<std::int16_t>(read16(rom, piece + 2));
        const int width = rom[piece + 6 - kRomBase], height = rom[piece + 7 - kRomBase];
        const std::uint32_t bytes = read32(rom, piece + 8);
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px) {
                const std::size_t at = bytes + ((py / 8) * (width / 8) + px / 8) * 32 + (py % 8) * 4 + (px % 8) / 2;
                const int ax = x + px, ay = y + py;
                if (at < source.size() && ax >= 0 && ay >= 0 && ax < kTwoTowersIcon && ay < kTwoTowersIcon)
                    art[ay][ax] = (source[at] >> ((px & 1) * 4)) & 0xF;
            }
    }

    Icon icon;
    icon.palette.resize(32);
    std::memcpy(icon.palette.data(), rom.data() + (paletteAddress - kRomBase), 32);
    auto color = [&](int slot) { return icon.palette[slot * 2] | icon.palette[slot * 2 + 1] << 8; };

    // Clear the bevel and the fill. The fill is the most common color inside the bevel. Clear
    // what the outer ring reaches through the ring's colors and the fill, then any fill left
    // between the item's parts (inside a necklace's loop).
    constexpr int kLast = kTwoTowersIcon - 1, kInner = 2;
    std::uint16_t ring = 0;
    int counts[16] = {};
    for (int i = 0; i < kTwoTowersIcon; ++i)
        ring |= 1u << art[0][i] | 1u << art[kLast][i] | 1u << art[i][0] | 1u << art[i][kLast];
    for (int i = kInner; i <= kLast - kInner; ++i)
        for (int edge : {kInner, kLast - kInner}) ++counts[art[i][edge]], ++counts[art[edge][i]];
    const int fill = static_cast<int>(std::max_element(std::begin(counts), std::end(counts)) - std::begin(counts));
    auto isFill = [&](int slot) { return color(slot) == color(fill); };
    bool seen[kTwoTowersIcon][kTwoTowersIcon] = {};
    std::vector<std::pair<int, int>> stack;
    for (int i = 0; i < kTwoTowersIcon; ++i)
        stack.insert(stack.end(), {{0, i}, {kLast, i}, {i, 0}, {i, kLast}});
    while (!stack.empty()) {
        const auto [y, x] = stack.back();
        stack.pop_back();
        if (x < 0 || y < 0 || x > kLast || y > kLast || seen[y][x]) continue;
        if (!(ring >> art[y][x] & 1) && !isFill(art[y][x])) continue;
        seen[y][x] = true;
        art[y][x] = 0;
        stack.insert(stack.end(), {{y - 1, x}, {y + 1, x}, {y, x - 1}, {y, x + 1}});
    }
    for (auto& row : art)
        for (std::uint8_t& pixel : row)
            if (pixel && isFill(pixel)) pixel = 0;
    std::uint16_t used = 1;
    for (const auto& row : art)
        for (std::uint8_t pixel : row) used |= 1u << pixel;
    // White and black go in slots the item does not use, else in its closest colors.
    auto slotFor = [&](std::uint16_t wanted) {
        for (int slot = 1; slot < 16; ++slot)
            if (!(used >> slot & 1)) {
                used |= 1u << slot;
                icon.palette[slot * 2] = wanted & 0xFF;
                icon.palette[slot * 2 + 1] = wanted >> 8;
                return slot;
            }
        int best = 1, bestDistance = 1 << 30;
        for (int slot = 1; slot < 16; ++slot) {
            int distance = 0;
            for (int shift : {0, 5, 10}) distance += std::abs((color(slot) >> shift & 31) - (wanted >> shift & 31));
            if (distance < bestDistance) best = slot, bestDistance = distance;
        }
        return best;
    };
    const int white = slotFor(kWhite), black = slotFor(kBlack);

    std::uint8_t pixels[kIconHeight][kIconWidth] = {};
    for (int x = 1; x < kFrameRight; ++x) pixels[0][x] = pixels[kFrameBottom][x] = white;
    for (int y = 1; y < kFrameBottom; ++y) pixels[y][0] = pixels[y][kFrameRight] = white;
    for (int x = 1; x < kFrameRight; ++x) pixels[1][x] = black;
    for (int y = 1; y < kFrameBottom; ++y) pixels[y][1] = black;
    for (int y = 0; y < kTwoTowersIcon; ++y)
        for (int x = 0; x < kTwoTowersIcon; ++x)
            if (art[y][x] && y < kIconHeight) pixels[y][x + kIconShiftX] = art[y][x];

    // Tiles row-major over the 4x3 tile grid: the 32x8 OBJ's 4 tiles, then the 32x16 OBJ's 8.
    icon.tiles.assign(kIconWidth * kIconHeight / 2, 0);
    for (int y = 0; y < kIconHeight; ++y)
        for (int x = 0; x < kIconWidth; ++x)
            icon.tiles[((y / 8) * (kIconWidth / 8) + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2] |=
                static_cast<std::uint8_t>(pixels[y][x] << ((x & 1) * 4));

    // One uncompressed frame, header then frame offset 2, laid out as ROTK's own icons.
    const std::uint16_t tileBytes = static_cast<std::uint16_t>(icon.tiles.size());
    icon.frames = {kIconWidth, kIconHeight, 0, 0, 0, 0, 1, 0,
                   static_cast<std::uint8_t>(tileBytes), static_cast<std::uint8_t>(tileBytes >> 8), 0, 0, 2, 0,
                   2, 0, kIconWidth, kIconHeight, 0, 0,
                   static_cast<std::uint8_t>(tileBytes), static_cast<std::uint8_t>(tileBytes >> 8),
                   0x00, 0x00, 0x14, 0x00, 0x00, 0x10, 0x18, 0x01};
    return icon;
}

// Skill icons: per hero, 11 BG assets (flags 0x70: tiles packed with the C64 pucrunch, no
// palette) of 4x4 tiles: a round frame (colors 11..15) around the art (1..10).
constexpr std::uint32_t kSkillIcons = 0x0832EA64u;
constexpr std::uint32_t kHeroCount = 6, kSkillCount = 11;
// The 16 colors the skill screen draws them with.
constexpr std::uint32_t kSkillIconPalette = 0x08A792E8u + 2;
constexpr int kSkillIconSize = 32;
// The art for ROTK's 32x24 skill icon, whose border is 1 pixel at the top and bottom and 2 at the
// sides, in its art shades 8 (brightest) .. 13.
constexpr int kSkillArtWidth = 32, kSkillArtHeight = 24;
constexpr std::uint32_t kSkillArtBytes = kSkillArtWidth * kSkillArtHeight / 2;
constexpr int kArtShadeFirst = 8, kArtShadeCount = 6;

// The Two Towers' BG tile decoder (ARM at ROM 0x08000264, copied to IWRAM): the C64 pucrunch
// unpacker. It builds an LZ source address byte by byte in its stack frame, as the 6502 did, and
// stores halfwords: a copy reading the byte still waiting for its pair reads what memory held
// (zero here) unless it starts on it.
std::vector<std::uint8_t> unpackTiles(const std::vector<std::uint8_t>& rom, std::uint32_t address)
{
    constexpr std::uint32_t kDest = 0x06000000u;
    std::vector<std::uint8_t> out;
    std::uint8_t frame[68] = {};
    std::uint32_t p = address - kRomBase + 6;
    auto byte = [&]() -> std::uint8_t { return p < rom.size() ? rom[p++] : 0; };
    std::memcpy(frame + 4, &kDest, 4);
    frame[0] = byte();
    p += 2;
    frame[8] = byte();
    frame[0xC] = static_cast<std::uint8_t>(8 - frame[8]);
    frame[0x10] = byte();
    frame[0x1C] = static_cast<std::uint8_t>(9 - frame[0x10]);
    frame[0x14] = byte();
    frame[0x18] = static_cast<std::uint8_t>(2 * frame[0x14] - 1);
    frame[0x20] = byte();
    p += 2;
    const std::uint8_t tableSize = byte();
    for (std::uint32_t i = 0; i < tableSize && 36 + i < sizeof frame; ++i) frame[36 + i] = byte();
    std::uint32_t bits = 128;
    auto bit = [&]() -> std::uint32_t {
        bits <<= 1;
        if ((bits & 0xFF) == 0) bits = byte() * 2u + 1u;
        return (bits >> 8) & 1u;
    };
    auto read = [&](std::uint32_t value, std::uint32_t n) {
        while (n--) value = value * 2 + bit();
        return value;
    };
    auto gamma = [&]() {
        std::uint32_t n = 1;
        while (bit()) {
            if (++n == frame[0x10]) break;
        }
        return read(1, n - 1);
    };
    while (out.size() < 0x4000) {
        const std::uint8_t escape = frame[0];
        const std::uint32_t head = read(0, frame[8]);
        if (head != escape) {
            out.push_back(static_cast<std::uint8_t>(read(head, frame[0xC])));
            continue;
        }
        std::uint32_t g = gamma();
        frame[4] = static_cast<std::uint8_t>(g);
        std::uint8_t low;
        if (g >> 1) {
            const std::uint32_t g2 = gamma();
            if (g2 == frame[0x18]) break;
            frame[5] = static_cast<std::uint8_t>(read(g2 - 1, frame[0x20]));
            low = static_cast<std::uint8_t>(read(frame[5], 8));
        } else if (bit() == 0) {
            frame[5] = 0;  // a short copy has one offset byte
            low = static_cast<std::uint8_t>(read(0, 8));
        } else if (bit() == 0) {
            frame[0] = static_cast<std::uint8_t>(read(0, frame[8]));
            out.push_back(static_cast<std::uint8_t>(read(escape, frame[0xC])));
            continue;
        } else {
            g = gamma();
            frame[4] = static_cast<std::uint8_t>(g);
            std::uint32_t high = 0;
            if (g >= frame[0x14]) {
                frame[4] = static_cast<std::uint8_t>(read(g, frame[0x1C]));
                high = gamma() - 1;
            }
            const std::uint32_t rank = gamma();
            const std::uint8_t value =
                rank < 32 ? frame[35 + rank] : static_cast<std::uint8_t>(read(rank, 3));
            out.insert(out.end(), frame[4] + 1 + (high << 8), value);
            continue;
        }
        const std::uint32_t length = frame[4] + 1u;
        const std::uint32_t next = kDest + static_cast<std::uint32_t>(out.size());
        const std::uint32_t sum = (next & 0xFF) + low;
        const std::uint32_t borrow = ((sum >> 8) & 1u) ^ 1u;
        frame[4] = static_cast<std::uint8_t>(sum);
        const std::uint32_t mid = (next >> 8) - frame[5] - borrow;
        frame[5] = static_cast<std::uint8_t>(mid);
        frame[6] = static_cast<std::uint8_t>(mid >> 8);
        std::uint32_t source;
        std::memcpy(&source, frame + 4, 4);
        source -= kDest;
        if (source + 1 == out.size() && (out.size() & 1)) {
            out.insert(out.end(), length, out.back());
            continue;
        }
        for (std::uint32_t k = 0; k < length; ++k) {
            const std::size_t at = source + k, stored = out.size() & ~std::size_t{1};
            out.push_back(at < stored ? out[at] : 0);
        }
    }
    return out;
}

// A Two Towers skill icon's art for ROTK's skill icon: frame and fill cleared, colors by brightness
// in ROTK's art shades, centered in 32x24 4bpp tiles (4 by 3, row by row).
std::vector<std::uint8_t> convertSkillArt(const std::vector<std::uint8_t>& rom, std::uint32_t asset)
{
    constexpr int kLast = kSkillIconSize - 1;
    const std::vector<std::uint8_t> tiles = unpackTiles(rom, asset + 4);
    std::uint8_t art[kSkillIconSize][kSkillIconSize] = {};
    for (int y = 0; y < kSkillIconSize; ++y)
        for (int x = 0; x < kSkillIconSize; ++x) {
            const std::size_t at = ((y / 8) * 4 + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2;
            if (at < tiles.size()) art[y][x] = (tiles[at] >> ((x & 1) * 4)) & 0xF;
        }
    // The frame colors and the fill the edge reaches are not art.
    constexpr std::uint16_t kBackground = 1u << 0 | 1u << 11 | 1u << 12 | 1u << 13 | 1u << 14 | 1u << 15;
    bool seen[kSkillIconSize][kSkillIconSize] = {};
    std::vector<std::pair<int, int>> stack;
    for (int i = 0; i < kSkillIconSize; ++i) stack.insert(stack.end(), {{0, i}, {kLast, i}, {i, 0}, {i, kLast}});
    while (!stack.empty()) {
        const auto [y, x] = stack.back();
        stack.pop_back();
        if (x < 0 || y < 0 || x > kLast || y > kLast || seen[y][x] || !(kBackground >> art[y][x] & 1)) continue;
        seen[y][x] = true;
        art[y][x] = 0;
        stack.insert(stack.end(), {{y - 1, x}, {y + 1, x}, {y, x - 1}, {y, x + 1}});
    }
    std::uint8_t shade[16];
    for (int i = 0; i < 16; ++i) {
        const int color = read16(rom, kSkillIconPalette + i * 2);
        const double luma = 0.3 * (color & 31) + 0.59 * (color >> 5 & 31) + 0.11 * (color >> 10 & 31);
        shade[i] = static_cast<std::uint8_t>(
            kArtShadeFirst + std::min(kArtShadeCount - 1, static_cast<int>((31 - luma) * kArtShadeCount / 32)));
    }
    int top = kSkillIconSize, bottom = -1, left = kSkillIconSize, right = -1;
    for (int y = 0; y < kSkillIconSize; ++y)
        for (int x = 0; x < kSkillIconSize; ++x)
            if (art[y][x]) {
                top = std::min(top, y), bottom = std::max(bottom, y);
                left = std::min(left, x), right = std::max(right, x);
            }
    std::vector<std::uint8_t> out(kSkillArtBytes, 0);
    if (bottom < 0) return out;
    const int dy = kSkillArtHeight / 2 - (top + bottom + 1) / 2, dx = kSkillArtWidth / 2 - (left + right) / 2;
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x) {
            const int ty = y + dy, tx = x + dx;
            if (!art[y][x] || ty < 1 || ty > kSkillArtHeight - 2 || tx < 2 || tx > kSkillArtWidth - 3) continue;
            out[((ty / 8) * (kSkillArtWidth / 8) + tx / 8) * 32 + (ty % 8) * 4 + (tx % 8) / 2] |=
                static_cast<std::uint8_t>(shade[art[y][x]] << ((tx & 1) * 4));
        }
    return out;
}

// Two Towers sprite animations the legacy skills draw, converted to ROTK's format, with the palette
// each is drawn with (Two Towers loads it separately), 0 for none.
struct SpriteSource {
    std::uint32_t animation, palette;
};
constexpr SpriteSource kSprites[] = {
    // Gandalf's Zap: the bolt by facing (down, down-side, side, up-side), and its impact
    {0x08050138u, 0x08CC6268u}, {0x08050148u, 0x08CC6268u}, {0x08050158u, 0x08CC6268u},
    {0x08050168u, 0x08CC6268u}, {0x080501C8u, 0x08CC6268u},
};
constexpr std::uint32_t kFrameHeadSize = 6;          // Two Towers: flags, pad, width, height, tile offset
constexpr std::uint32_t kRotkFrameHeadSize = 8;      // ROTK adds the frame's tile bytes
// Two Towers frames keep hotspots before trigger boxes of 8 bytes; ROTK the boxes (6 bytes) first.
constexpr std::uint32_t kHotspotSize = 2, kTriggerBoxSize = 8, kRotkTriggerBoxSize = 6;
constexpr std::uint8_t kFramePieceCount = 0x1F, kFrameRl = 0x20, kFrameLz77 = 0x40;

// ROTK frame set and 4bpp tiles of one Two Towers animation: same header; hotspots and trigger boxes
// reordered; tiles
// decompressed (the Two Towers LZ77, or copied when raw) and its 12-byte OAM pieces packed
// into ROTK's {x:9, y:9, size:2, shape:2, tile:10}.
struct Sprite {
    std::vector<std::uint8_t> frames, tiles;
    std::uint8_t duration = 0;
};

Sprite convertSprite(const std::vector<std::uint8_t>& rom, std::uint32_t animation)
{
    Sprite sprite;
    const std::uint32_t tileBase = read32(rom, animation), frameSet = read32(rom, animation + 4);
    sprite.duration = rom[animation + 12 - kRomBase];
    const std::uint16_t frameCount = read16(rom, frameSet + 6);
    const std::uint8_t hotspots = rom[frameSet + 10 - kRomBase], boxes = rom[frameSet + 11 - kRomBase];
    sprite.frames.assign(rom.begin() + (frameSet - kRomBase), rom.begin() + (frameSet - kRomBase) + kFrameSetHeaderSize);
    sprite.frames.resize(kFrameSetHeaderSize + frameCount * 2u + (frameCount & 1u) * 2u, 0);
    for (std::uint32_t f = 0; f < frameCount; ++f) {
        const std::uint32_t frame = frameSet + kFrameSetHeaderSize + read16(rom, frameSet + kFrameSetHeaderSize + f * 2);
        const std::uint8_t flags = rom[frame - kRomBase];
        const std::uint32_t pieces = flags & kFramePieceCount;
        const std::uint32_t boxBytes = hotspots * kHotspotSize + boxes * kTriggerBoxSize;
        const std::uint32_t firstPiece = frame + kFrameHeadSize + boxBytes;
        std::uint32_t bytes = 0;
        for (std::uint32_t k = 0; k < pieces; ++k) {
            const std::uint32_t piece = firstPiece + k * kFramePieceSize;
            bytes = std::max(bytes, read32(rom, piece + 8) + rom[piece + 6 - kRomBase] * rom[piece + 7 - kRomBase] / 2u);
        }
        const std::uint32_t source = tileBase + read16(rom, frame + 4);
        std::vector<std::uint8_t> tiles = (flags & kFrameLz77) ? decompressTiles(rom, source)
                                                               : std::vector<std::uint8_t>(rom.begin() + (source - kRomBase), rom.begin() + (source - kRomBase) + bytes);
        tiles.resize(bytes, 0);
        const std::uint32_t tileOffset = static_cast<std::uint32_t>(sprite.tiles.size());
        sprite.tiles.insert(sprite.tiles.end(), tiles.begin(), tiles.end());

        const std::uint16_t offset = static_cast<std::uint16_t>(sprite.frames.size() - kFrameSetHeaderSize);
        std::memcpy(sprite.frames.data() + kFrameSetHeaderSize + f * 2, &offset, 2);
        sprite.frames.insert(sprite.frames.end(), {static_cast<std::uint8_t>(pieces), 0, rom[frame + 2 - kRomBase], rom[frame + 3 - kRomBase],
                                                   static_cast<std::uint8_t>(tileOffset), static_cast<std::uint8_t>(tileOffset >> 8),
                                                   static_cast<std::uint8_t>(bytes), static_cast<std::uint8_t>(bytes >> 8)});
        const std::uint32_t hotspotData = frame + kFrameHeadSize, boxData = hotspotData + hotspots * kHotspotSize;
        for (std::uint32_t b = 0; b < boxes; ++b) {
            const auto box = rom.begin() + (boxData + b * kTriggerBoxSize - kRomBase);
            sprite.frames.insert(sprite.frames.end(), box, box + kRotkTriggerBoxSize);
        }
        sprite.frames.insert(sprite.frames.end(), rom.begin() + (hotspotData - kRomBase), rom.begin() + (boxData - kRomBase));
        for (std::uint32_t k = 0; k < pieces; ++k) {
            const std::uint32_t piece = firstPiece + k * kFramePieceSize;
            const std::uint32_t x = read16(rom, piece) & 0x1FFu, y = read16(rom, piece + 2) & 0x1FFu;
            const std::uint32_t size = rom[piece + 4 - kRomBase] & 3u, shape = rom[piece + 5 - kRomBase] & 3u;
            const std::uint32_t tile = read32(rom, piece + 8) / 32u;
            const std::uint32_t packed = x | y << 9 | size << 18 | shape << 20 | tile << 22;
            sprite.frames.insert(sprite.frames.end(), {static_cast<std::uint8_t>(packed), static_cast<std::uint8_t>(packed >> 8),
                                                       static_cast<std::uint8_t>(packed >> 16), static_cast<std::uint8_t>(packed >> 24)});
        }
    }
    return sprite;
}

// The Two Towers save's character slots, zeroed for an empty or damaged one.
std::vector<std::uint8_t> loadSaveSlots()
{
    fs::path path = g_romPath;
    path.replace_extension(".sav");
    if (const char* given = std::getenv("ROTK_PC_TWO_TOWERS_SAV")) path = given;
    const std::vector<std::uint8_t> file = load(path);
    std::vector<std::uint8_t> slots(std::size(kSaveSlotBlocks) * kSaveSlotSize, 0);
    if (file.size() != kSaveSize) return slots;
    std::vector<std::uint8_t> image(file.size());
    for (std::size_t i = 0; i < file.size(); ++i)
        image[i] = file[(i & ~std::size_t{kSaveBlock - 1}) + (kSaveBlock - 1 - (i & (kSaveBlock - 1)))];
    int used = 0;
    for (std::size_t n = 0; n < std::size(kSaveSlotBlocks); ++n) {
        const std::uint8_t* slot = image.data() + kSaveSlotBlocks[n] * kSaveBlock;
        std::uint16_t sum = 0;
        bool blank = true;
        for (std::uint32_t i = 0; i < kSaveSlotSize; i += 2) {
            sum = static_cast<std::uint16_t>(sum + (slot[i] | slot[i + 1] << 8));
            blank = blank && slot[i] == 0 && slot[i + 1] == 0;
        }
        if (sum != 0 || blank) continue;
        std::memcpy(slots.data() + n * kSaveSlotSize, slot, kSaveSlotSize);
        ++used;
    }
    std::fprintf(stderr, "[rotk] The Two Towers save: %s, %d hero(es)\n", path.string().c_str(), used);
    return slots;
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
    const std::vector<std::uint8_t> slots = loadSaveSlots();
    putPointer(kSaveSlotsField, append(slots.data(), slots.size()));
    put32(kSaveSlotsField + 4, static_cast<std::uint32_t>(std::size(kSaveSlotBlocks)));

    std::vector<std::uint8_t> records(kBaseCount * kIconRecordSize, 0);
    const std::uint32_t icons = append(records.data(), records.size());
    putPointer(kIconsField, icons);
    for (std::uint32_t i = 0; i < kBaseCount; ++i) {
        const Icon icon = convertIcon(rom, kBaseIcons + i * kIconRecordSize);
        if (icon.tiles.empty()) continue;
        const std::uint32_t at = icons + i * kIconRecordSize;
        putPointer(at, append(icon.tiles.data(), icon.tiles.size()));
        putPointer(at + 4, append(icon.frames.data(), icon.frames.size()));
        putPointer(at + 8, append(icon.palette.data(), icon.palette.size()));
    }

    std::vector<std::uint8_t> skillArt;
    for (std::uint32_t hero = 0; hero < kHeroCount; ++hero)
        for (std::uint32_t skill = 0; skill < kSkillCount; ++skill) {
            const std::vector<std::uint8_t> art =
                convertSkillArt(rom, read32(rom, kSkillIcons + (hero * kSkillCount + skill) * 4));
            skillArt.insert(skillArt.end(), art.begin(), art.end());
        }
    putPointer(kSkillArtField, append(skillArt.data(), skillArt.size()));

    // {Two Towers address, ROTK SpriteAnimation} per converted animation
    std::vector<std::uint8_t> index(std::size(kSprites) * 8, 0);
    const std::uint32_t sprites = append(index.data(), index.size());
    putPointer(kSpritesField, sprites);
    put32(kSpritesField + 4, static_cast<std::uint32_t>(std::size(kSprites)));
    for (std::size_t i = 0; i < std::size(kSprites); ++i) {
        const Sprite sprite = convertSprite(rom, kSprites[i].animation);
        const std::uint32_t tiles = append(sprite.tiles.data(), sprite.tiles.size());
        const std::uint32_t frames = append(sprite.frames.data(), sprite.frames.size());
        const std::uint8_t record[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, sprite.duration, 0, 0, 0};
        const std::uint32_t animation = append(record, sizeof record);
        putPointer(animation, tiles);
        putPointer(animation + 4, frames);
        if (kSprites[i].palette) putPointer(animation + 8, append(rom.data() + (kSprites[i].palette - kRomBase), 32));
        const std::uint32_t at = sprites + static_cast<std::uint32_t>(i) * 8;
        put32(at, kSprites[i].animation);
        putPointer(at + 4, animation);
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

bool twoTowersPresent(std::uint32_t address)
{
    return bus_read_u32(address) == kMagic;
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
