#include "rotk_sprites.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "gba_bus.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace rotk {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kRomBase = 0x08000000u;

// The Two Towers: the pose table (hero * 6 + pose -> 5 facing pointers -> 8-byte {tiles, frame set}
// records) and the hero palette pointers.
constexpr std::uint32_t kPoseTable = 0x08329664u;
constexpr std::uint32_t kHeroPalettes = 0x0832B248u + 0x33u * 4u;
constexpr int kHeroes = 6, kPoses = 6, kFacings = 5;
// The weapon part's pose table: weapon kind * 6 + pose, laid out like the pose table. The part shows
// the body's frame, so with ROTK bodies (more frames) it gets a blank animation with enough frames.
constexpr std::uint32_t kWeaponPoseTable = 0x083314A0u;
constexpr int kWeaponEntries = 84;
constexpr std::uint32_t kBlankFrames = 64;
enum Pose { kWalk, kStand, kAttack, kDeath, kFlinch, kCast };

// ROTK: per hero a bank of part * 7 + pose entries, each 5 inline 16-byte SpriteAnimation records.
constexpr std::uint32_t kMoveBanks = 0x08282898u, kMeleeBanks = 0x082828B8u, kCastBanks = 0x082828D8u;
constexpr std::uint32_t kObjPalettes = 0x082830B0u;
constexpr std::uint32_t kHeroPaletteBase = 0x21u;  // ObjPalettes id of hero h
constexpr std::uint32_t kRecordSize = 16u;

struct Source {
    std::uint32_t banks;
    std::uint32_t pose;
};
// Each Two Towers pose's ROTK animation (body part).
constexpr Source kSources[kPoses] = {
    {kMoveBanks, 6},   // walk
    {kMoveBanks, 4},   // stand
    {kMeleeBanks, 0},  // attack: the first swing
    {kMoveBanks, 1},   // death
    {kMoveBanks, 3},   // flinch: the stagger
    {kCastBanks, 0},   // cast
};

// Converted data in host RAM, past the plain host's own use of its first page.
constexpr std::uint32_t kDataBase = 0x12100000u;
constexpr std::uint32_t kMagic = 0x53544B52u;  // "RKTS"

struct State {
    bool enabled = false;
    Bytes rotk;
    Bytes image;  // written at kDataBase
    std::uint32_t poseTable = 0, palettes = 0, blankWeapon = 0;
};
State g_state;

std::uint32_t r8(const Bytes& rom, std::uint32_t a) { return a - kRomBase < rom.size() ? rom[a - kRomBase] : 0u; }
std::uint32_t r16(const Bytes& rom, std::uint32_t a) { return r8(rom, a) | r8(rom, a + 1) << 8; }
std::uint32_t r32(const Bytes& rom, std::uint32_t a) { return r16(rom, a) | r16(rom, a + 2) << 16; }

// ROTK's decompress_lz77 (0x0800D1D4): runs are 3, copies 2 and long runs 0x42 longer than in the
// Two Towers' variant.
Bytes lz77(const Bytes& rom, std::uint32_t p)
{
    Bytes out;
    auto at = [&](std::uint32_t i) -> std::uint8_t { return i < rom.size() ? rom[i] : 0; };
    while (out.size() < 0x4000) {
        const std::uint32_t c = at(p++);
        if (c == 0) break;
        if (c < 0x40) {
            for (std::uint32_t i = 0; i < c; ++i) out.push_back(at(p++));
        } else if (c < 0x80) {
            out.insert(out.end(), (c & 0x3Fu) + 3u, at(p++));
        } else {
            const std::uint32_t o = at(p++);
            const std::uint32_t d = (c & 0x7Fu) << 3 | o >> 5, n = o & 0x1Fu;
            if (n == 0) {
                out.insert(out.end(), d + 0x42u, at(p++));
            } else {
                for (std::uint32_t i = 0; i < n + 2u; ++i) out.push_back(d && d <= out.size() ? out[out.size() - d] : 0);
            }
        }
    }
    return out;
}

Bytes runLength(const Bytes& rom, std::uint32_t p)
{
    auto at = [&](std::uint32_t i) -> std::uint8_t { return i < rom.size() ? rom[i] : 0; };
    const std::uint32_t size = (at(p) | at(p + 1) << 8 | at(p + 2) << 16 | at(p + 3) << 24) >> 8;
    p += 4;
    Bytes out;
    while (out.size() < size) {
        const std::uint32_t f = at(p++);
        if (f & 0x80u) out.insert(out.end(), (f & 0x7Fu) + 3u, at(p++));
        else for (std::uint32_t i = 0; i <= (f & 0x7Fu); ++i) out.push_back(at(p++));
    }
    out.resize(size);
    return out;
}

constexpr int kObjWidth[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
constexpr int kObjHeight[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

void put16(Bytes& b, std::uint32_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8 & 0xFF); }
void put32(Bytes& b, std::uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
void align4(Bytes& b) { while (b.size() % 4) b.push_back(0); }

struct Converted {
    Bytes tiles, frameSet;
};

// A Two Towers attack frame's attack box, to give the converted swing.
struct AttackBox {
    bool found = false;
    std::uint32_t frame = 0, frames = 1;
    std::uint8_t box[8] = {};
};

AttackBox ttAttackBox(std::uint32_t animation)
{
    AttackBox result;
    const std::uint32_t frameSet = bus_read_u32(animation + 4);
    const std::uint32_t count = bus_read_u16(frameSet + 6), hotspots = bus_read_u8(frameSet + 10),
                        boxes = bus_read_u8(frameSet + 11);
    result.frames = count ? count : 1;
    if (boxes < 2) return result;
    for (std::uint32_t f = 0; f < count; ++f) {
        const std::uint32_t frame = frameSet + 12 + bus_read_u16(frameSet + 12 + f * 2);
        const std::uint32_t box = frame + 6 + hotspots * 2 + 8;  // the second box
        if (!bus_read_u32(box) && !bus_read_u32(box + 4)) continue;
        result.found = true;
        result.frame = f;
        for (int i = 0; i < 8; ++i) result.box[i] = bus_read_u8(box + i);
        break;
    }
    return result;
}

// A ROTK animation in the Two Towers' format (raw tiles, 12-byte pieces, 8-byte boxes), with an
// attack box on the strike frame when one is given.
Converted convert(const Bytes& rom, std::uint32_t animation, const AttackBox& attack)
{
    Converted out;
    const std::uint32_t tilesBase = r32(rom, animation), frameSet = r32(rom, animation + 4);
    const std::uint32_t count = r16(rom, frameSet + 6), hotspots = r8(rom, frameSet + 10), boxes = r8(rom, frameSet + 11);
    const std::uint32_t outBoxes = attack.found ? std::max<std::uint32_t>(boxes, 2) : boxes;
    const std::uint32_t strike = attack.found ? (attack.frame * count + attack.frames / 2) / attack.frames : 0;
    std::vector<Bytes> frames;
    for (std::uint32_t f = 0; f < count; ++f) {
        const std::uint32_t frame = frameSet + 12 + r16(rom, frameSet + 12 + f * 2);
        const std::uint32_t flags = r8(rom, frame), pieces = flags & 0x1Fu;
        const std::uint32_t tileOffset = r16(rom, frame + 4), tileBytes = r16(rom, frame + 6);
        const std::uint32_t source = tilesBase + tileOffset - kRomBase;
        Bytes data = (flags & 0x20u)   ? runLength(rom, source)
                     : (flags & 0x40u) ? lz77(rom, source)
                                       : Bytes(rom.begin() + std::min<std::size_t>(source, rom.size()),
                                               rom.begin() + std::min<std::size_t>(source + tileBytes, rom.size()));
        data.resize(tileBytes, 0);
        const std::uint32_t offset = static_cast<std::uint32_t>(out.tiles.size());
        out.tiles.insert(out.tiles.end(), data.begin(), data.end());
        align4(out.tiles);

        Bytes b = {static_cast<std::uint8_t>(pieces), 0, static_cast<std::uint8_t>(r8(rom, frame + 2)),
                   static_cast<std::uint8_t>(r8(rom, frame + 3))};
        put16(b, offset);
        std::uint32_t p = frame + 8;
        const std::uint32_t boxData = p;
        p += 6 * boxes;
        for (std::uint32_t h = 0; h < hotspots * 2; ++h) b.push_back(static_cast<std::uint8_t>(r8(rom, p + h)));
        p += 2 * hotspots;
        for (std::uint32_t k = 0; k < outBoxes; ++k) {
            if (k < boxes) {
                for (std::uint32_t i = 0; i < 6; ++i) b.push_back(static_cast<std::uint8_t>(r8(rom, boxData + 6 * k + i)));
                b.push_back(0);
                b.push_back(0);
            } else if (k == 1 && f == strike) {
                b.insert(b.end(), attack.box, attack.box + 8);
            } else {
                b.insert(b.end(), 8, 0);
            }
        }
        for (std::uint32_t k = 0; k < pieces; ++k) {
            const std::uint32_t v = r32(rom, p + 4 * k);
            const std::uint32_t x = v & 0x1FFu, y = v >> 9 & 0x1FFu, size = v >> 18 & 3u, shape = v >> 20 & 3u;
            const std::uint32_t tile = v >> 22 & 0x3FFu;
            // signed 16-bit in the Two Towers' pieces, 9-bit in ROTK's
            put16(b, x & 0x100u ? x | 0xFE00u : x);
            put16(b, y & 0x100u ? y | 0xFE00u : y);
            b.push_back(static_cast<std::uint8_t>(size));
            b.push_back(static_cast<std::uint8_t>(shape));
            b.push_back(static_cast<std::uint8_t>(shape < 3 ? kObjWidth[shape][size] : 8));
            b.push_back(static_cast<std::uint8_t>(shape < 3 ? kObjHeight[shape][size] : 8));
            put32(b, tile * 32u);
        }
        frames.push_back(std::move(b));
    }
    for (std::uint32_t i = 0; i < 12; ++i) out.frameSet.push_back(static_cast<std::uint8_t>(r8(rom, frameSet + i)));
    out.frameSet[11] = static_cast<std::uint8_t>(outBoxes);
    Bytes body;
    for (const Bytes& frame : frames) {
        put16(out.frameSet, 2 * count + static_cast<std::uint32_t>(body.size()));
        body.insert(body.end(), frame.begin(), frame.end());
        if (body.size() % 2) body.push_back(0);
    }
    out.frameSet.insert(out.frameSet.end(), body.begin(), body.end());
    return out;
}

// Guest address of the bytes appended to the image.
std::uint32_t append(Bytes& image, const Bytes& data)
{
    align4(image);
    const std::uint32_t at = kDataBase + static_cast<std::uint32_t>(image.size());
    image.insert(image.end(), data.begin(), data.end());
    return at;
}

void build()
{
    const Bytes& rom = g_state.rotk;
    Bytes& image = g_state.image;
    image.clear();
    put32(image, kMagic);
    Bytes poseTable(kHeroes * kPoses * 4, 0), palettes;
    for (int hero = 0; hero < kHeroes; ++hero) {
        for (int pose = 0; pose < kPoses; ++pose) {
            const Source& source = kSources[pose];
            const std::uint32_t entry = r32(rom, r32(rom, source.banks + hero * 4u) + source.pose * 4u);
            Bytes facings;
            const std::uint32_t ttFacings = bus_read_u32(kPoseTable + (hero * kPoses + pose) * 4u);
            for (int facing = 0; facing < kFacings; ++facing) {
                const AttackBox attack = pose == kAttack ? ttAttackBox(bus_read_u32(ttFacings + facing * 4u)) : AttackBox{};
                const Converted c = convert(rom, entry + facing * kRecordSize, attack);
                Bytes record;
                put32(record, append(image, c.tiles));
                put32(record, append(image, c.frameSet));
                put32(facings, append(image, record));
            }
            const std::uint32_t array = append(image, facings);
            std::memcpy(poseTable.data() + (hero * kPoses + pose) * 4, &array, 4);
        }
        const std::uint32_t palette = r32(rom, kObjPalettes + (kHeroPaletteBase + hero) * 4u);
        Bytes colors;
        for (std::uint32_t i = 0; i < 32; ++i) colors.push_back(static_cast<std::uint8_t>(r8(rom, palette + i)));
        put32(palettes, append(image, colors));
    }
    // the blank weapon: one transparent 8x8 piece in every frame
    Bytes blankTiles(32, 0), blankSet = {8, 8, 0, 0, 0, 0}, blankFacings, blankRecord;
    put16(blankSet, kBlankFrames);
    put16(blankSet, 32);
    blankSet.push_back(0);
    blankSet.push_back(0);
    const Bytes blankFrame = {1, 0, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0};
    for (std::uint32_t f = 0; f < kBlankFrames; ++f) put16(blankSet, 2 * kBlankFrames);
    blankSet.insert(blankSet.end(), blankFrame.begin(), blankFrame.end());
    put32(blankRecord, append(image, blankTiles));
    put32(blankRecord, append(image, blankSet));
    const std::uint32_t record = append(image, blankRecord);
    for (int facing = 0; facing < kFacings; ++facing) put32(blankFacings, record);
    g_state.blankWeapon = append(image, blankFacings);
    g_state.poseTable = append(image, poseTable);
    g_state.palettes = append(image, palettes);
    std::fprintf(stderr, "[ttt] ROTK hero sprites: %zu KiB\n", image.size() / 1024);
}

// The image in host RAM, rewritten when the boot or a reset cleared it.
void ensureWritten()
{
    if (bus_read_u32(kDataBase) == kMagic) return;
    for (std::size_t i = 0; i < g_state.image.size(); ++i)
        bus_write_u8(kDataBase + static_cast<std::uint32_t>(i), g_state.image[i]);
}

int romRead32(std::uint32_t address, std::uint32_t, std::uint32_t* out)
{
    if (address >= kPoseTable && address < kPoseTable + kHeroes * kPoses * 4u) {
        ensureWritten();
        *out = bus_read_u32(g_state.poseTable + (address - kPoseTable));
        return 1;
    }
    if (address >= kWeaponPoseTable && address < kWeaponPoseTable + kWeaponEntries * 4u) {
        ensureWritten();
        *out = g_state.blankWeapon;
        return 1;
    }
    if (address >= kHeroPalettes && address < kHeroPalettes + kHeroes * 4u) {
        ensureWritten();
        *out = bus_read_u32(g_state.palettes + (address - kHeroPalettes));
        return 1;
    }
    return 0;
}

void reset()
{
    // built from the Two Towers' own tables, so before the override answers for them
    gba::g_rom_read32_override = nullptr;
    if (!g_state.enabled) return;
    if (g_state.image.empty()) build();
    gba::g_rom_read32_override = romRead32;
}

}  // namespace

void rotkSpritesRegister()
{
    const char* on = std::getenv("ROTK_TTT_ROTK_SPRITES");
    const char* path = std::getenv("ROTK_TTT_ROTK_ROM");
    if (!on || std::strcmp(on, "1") != 0 || !path) return;
    std::ifstream file(path, std::ios::binary);
    g_state.rotk.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (g_state.rotk.size() < 0x00400000u) {
        std::fprintf(stderr, "[ttt] ROTK hero sprites: no ROTK ROM at %s\n", path);
        return;
    }
    g_state.enabled = true;
    gba_mod_register_reset_callback(reset);
}

}  // namespace rotk
