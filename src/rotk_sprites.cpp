#include "rotk_sprites.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
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
// Each weapon kind's hero and ROTK hand part (0: no art). The Two Towers picks the kind per hero
// when it spawns the weapon actor (player 0x0802F3BC, Gandalf's hat 0x0802F720, NPC heroes' main
// hand 0x08042310 and off hand 0x08043238), so a kind is always drawn with its hero's body. Hands,
// fists and the hat are part of ROTK's body art. Drawn in ROTK's weapons palette (OBJ bank 2)
// remapped to the hero's colors, as the Two Towers draws the weapon part in the hero's palette.
// The weapon actor's box makes the melee hits (callback 0x08010ED4), so every kind keeps one.
struct WeaponSource {
    int hero;
    std::uint32_t part;
};
constexpr WeaponSource kWeaponSources[] = {
    {0, 0},  // 0 Frodo's off hand
    {0, 1},  // 1 Frodo's sword
    {1, 1},  // 2 Legolas's bow
    {2, 1},  // 3 Aragorn's sword
    {2, 0},  // 4 Aragorn's off hand (NPC)
    {2, 0},  // 5 Aragorn's off-hand long sword (NPC)
    {3, 0},  // 6 Gandalf the Grey's hat
    {3, 1},  // 7 Gandalf's sword (NPC main hand)
    {3, 2},  // 8 Gandalf's staff
    {4, 1},  // 9 Eowyn's sword
    {4, 0},  // 10 Eowyn's off hand (NPC)
    {5, 1},  // 11 Gimli's axe
    {5, 0},  // 12 Gimli's off hand (NPC)
    {5, 0},  // 13 Gimli's axe (second)
};
static_assert(std::size(kWeaponSources) * 6 == kWeaponEntries);
constexpr std::uint32_t kWeaponsPalette = 2;
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

// The heroes' special animations (skills), which their code loads as literals: each Two Towers
// facing array and the ROTK animation (bank, pose) drawn in its place.
struct Special {
    std::uint32_t array;
    int hero;
    Source source;
};
constexpr Special kSpecials[] = {
    // Frodo
    {0x0832923Cu, 0, {kCastBanks, 0}},  {0x08329250u, 0, {kMeleeBanks, 0}}, {0x08329264u, 0, {kCastBanks, 3}},
    {0x08329278u, 0, {kMeleeBanks, 1}}, {0x0832928Cu, 0, {kMeleeBanks, 2}}, {0x083292A0u, 0, {kMeleeBanks, 2}},
    {0x083292B4u, 0, {kMeleeBanks, 4}}, {0x083292C8u, 0, {kMeleeBanks, 2}}, {0x083292DCu, 0, {kMeleeBanks, 0}},
    {0x083292F0u, 0, {kCastBanks, 0}},
    // Legolas
    {0x0832937Cu, 1, {kMeleeBanks, 4}}, {0x08329390u, 1, {kMeleeBanks, 0}},
    // Aragorn
    {0x083293E0u, 2, {kMeleeBanks, 2}}, {0x0832941Cu, 2, {kMeleeBanks, 4}}, {0x08329444u, 2, {kCastBanks, 0}},
    {0x08329458u, 2, {kMeleeBanks, 5}}, {0x0832946Cu, 2, {kMeleeBanks, 0}},
    // Gandalf
    {0x083294F8u, 3, {kCastBanks, 0}},  {0x0832950Cu, 3, {kCastBanks, 1}},
    // Gimli
    {0x08329584u, 5, {kMeleeBanks, 4}}, {0x08329598u, 5, {kMeleeBanks, 5}},
    // Eowyn
    {0x08329624u, 4, {kMeleeBanks, 2}}, {0x08329638u, 4, {kMeleeBanks, 4}},
};
constexpr std::uint32_t kCodeStart = 0x08000000u, kCodeEnd = 0x08060000u;

// Converted data in host RAM, past the plain host's own use of its first page.
constexpr std::uint32_t kDataBase = 0x12100000u;
constexpr std::uint32_t kMagic = 0x53544B52u;  // "RKTS"

struct State {
    bool enabled = false;
    Bytes rotk;
    Bytes image;  // written at kDataBase
    std::uint32_t poseTable = 0, palettes = 0, blankWeapon = 0;
    std::uint32_t weaponTable[kWeaponEntries] = {};
    // code literals that load a special animation, and the converted facing array each answers with
    std::vector<std::pair<std::uint32_t, std::uint32_t>> literals;
    std::uint32_t literalsLow = ~0u, literalsHigh = 0;
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

using Box = std::array<std::uint8_t, 8>;

// One box slot of a Two Towers animation, per frame (all zero when the frame has none).
struct TtBoxes {
    std::vector<Box> frames;
    bool any = false;
};

TtBoxes ttBoxes(std::uint32_t animation, std::uint32_t slot)
{
    TtBoxes result;
    const std::uint32_t frameSet = bus_read_u32(animation + 4);
    const std::uint32_t count = bus_read_u16(frameSet + 6), hotspots = bus_read_u8(frameSet + 10),
                        boxes = bus_read_u8(frameSet + 11);
    result.frames.resize(count);
    if (slot >= boxes) return result;
    for (std::uint32_t f = 0; f < count; ++f) {
        const std::uint32_t box = frameSet + 12 + bus_read_u16(frameSet + 12 + f * 2) + 6 + hotspots * 2 + slot * 8;
        for (int i = 0; i < 8; ++i) result.frames[f][i] = static_cast<std::uint8_t>(bus_read_u8(box + i));
        result.any = result.any || bus_read_u32(box) || bus_read_u32(box + 4);
    }
    return result;
}

// The boxes spread over outCount frames: each boxed frame lands on the frame at the same point in
// the animation.
std::vector<Box> scaleBoxes(const TtBoxes& tt, std::uint32_t outCount)
{
    std::vector<Box> out(outCount, Box{});
    const std::uint32_t count = static_cast<std::uint32_t>(tt.frames.size());
    if (!tt.any || !count || !outCount) return out;
    for (std::uint32_t f = 0; f < count; ++f)
        if (tt.frames[f] != Box{}) out[std::min(outCount - 1, (2 * f + 1) * outCount / (2 * count))] = tt.frames[f];
    return out;
}

// How a converted animation gets its boxes. Body: ROTK's boxes, and the Two Towers' box in slot 1
// (its attack box). Weapon: one slot, ROTK's strike box when the animation has one, else the Two
// Towers' box.
struct BoxPlan {
    bool weapon = false;
    TtBoxes tt;
};

// ROTK frame f's box k (6 bytes) as a Two Towers box (8 bytes).
Box rotkBox(const Bytes& rom, std::uint32_t frameSet, std::uint32_t f, std::uint32_t k)
{
    const std::uint32_t frame = frameSet + 12 + r16(rom, frameSet + 12 + f * 2);
    const std::uint32_t box = frame + 8 + 2 * r8(rom, frameSet + 10) + 6 * k;
    Box b = {};
    for (std::uint32_t i = 0; i < 6; ++i) b[i] = static_cast<std::uint8_t>(r8(rom, box + i));
    return b;
}

// Each output frame's boxes under the plan.
std::vector<std::vector<Box>> planBoxes(const Bytes& rom, std::uint32_t frameSet, const BoxPlan& plan,
                                        std::uint32_t outCount)
{
    const std::uint32_t count = r16(rom, frameSet + 6), boxes = r8(rom, frameSet + 11);
    const std::vector<Box> tt = scaleBoxes(plan.tt, outCount);
    std::vector<std::vector<Box>> out(outCount);
    if (plan.weapon) {
        bool own = false;
        for (std::uint32_t f = 0; f < count && !own; ++f)
            for (std::uint32_t k = 0; k < boxes && !own; ++k) own = rotkBox(rom, frameSet, f, k) != Box{};
        for (std::uint32_t f = 0; f < outCount; ++f) {
            Box b = {};
            if (!own) b = tt[f];
            else if (f < count)
                for (std::uint32_t k = 0; k < boxes && b == Box{}; ++k) b = rotkBox(rom, frameSet, f, k);
            out[f] = {b};
        }
        return out;
    }
    const std::uint32_t slots = plan.tt.any ? std::max<std::uint32_t>(boxes, 2) : boxes;
    for (std::uint32_t f = 0; f < outCount; ++f) {
        out[f].assign(slots, Box{});
        for (std::uint32_t k = 0; k < boxes; ++k) out[f][k] = rotkBox(rom, frameSet, std::min(f, count - 1), k);
        if (plan.tt.any && boxes < 2) out[f][1] = tt[f];
    }
    return out;
}

// A ROTK animation in the Two Towers' format (raw tiles, 12-byte pieces, 8-byte boxes), boxes as
// planned, its colors through remap when given (16 entries) and at least minFrames frames (the last
// repeated).
Converted convert(const Bytes& rom, std::uint32_t animation, const BoxPlan& plan,
                  const std::uint8_t* remap = nullptr, std::uint32_t minFrames = 0)
{
    Converted out;
    const std::uint32_t tilesBase = r32(rom, animation), frameSet = r32(rom, animation + 4);
    const std::uint32_t count = r16(rom, frameSet + 6), hotspots = r8(rom, frameSet + 10), boxes = r8(rom, frameSet + 11);
    const std::uint32_t outCount = std::max(count, minFrames);
    const auto frameBoxes = planBoxes(rom, frameSet, plan, outCount);
    const std::uint32_t outBoxes = outCount ? static_cast<std::uint32_t>(frameBoxes[0].size()) : boxes;
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
        if (remap)
            for (std::uint8_t& byte : data) byte = static_cast<std::uint8_t>(remap[byte & 15] | remap[byte >> 4] << 4);
        const std::uint32_t offset = static_cast<std::uint32_t>(out.tiles.size());
        out.tiles.insert(out.tiles.end(), data.begin(), data.end());
        align4(out.tiles);

        Bytes b = {static_cast<std::uint8_t>(pieces), 0, static_cast<std::uint8_t>(r8(rom, frame + 2)),
                   static_cast<std::uint8_t>(r8(rom, frame + 3))};
        put16(b, offset);
        // ROTK frames: hotspots, 6-byte trigger boxes, then pieces
        std::uint32_t p = frame + 8;
        for (std::uint32_t h = 0; h < hotspots * 2; ++h) b.push_back(static_cast<std::uint8_t>(r8(rom, p + h)));
        p += 2 * hotspots;
        p += 6 * boxes;
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
    while (!frames.empty() && frames.size() < outCount) frames.push_back(frames.back());
    // each frame: head and hotspots, its boxes, then pieces
    for (std::uint32_t f = 0; f < frames.size(); ++f) {
        Bytes boxBytes;
        for (const Box& box : frameBoxes[f]) boxBytes.insert(boxBytes.end(), box.begin(), box.end());
        frames[f].insert(frames[f].begin() + 6 + 2 * hotspots, boxBytes.begin(), boxBytes.end());
    }
    for (std::uint32_t i = 0; i < 12; ++i) out.frameSet.push_back(static_cast<std::uint8_t>(r8(rom, frameSet + i)));
    out.frameSet[6] = static_cast<std::uint8_t>(outCount);
    out.frameSet[7] = static_cast<std::uint8_t>(outCount >> 8);
    out.frameSet[11] = static_cast<std::uint8_t>(outBoxes);
    Bytes body;
    for (const Bytes& frame : frames) {
        put16(out.frameSet, 2 * outCount + static_cast<std::uint32_t>(body.size()));
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

// The 5 facings of a ROTK bank entry converted, as a facing array; attack boxes from ttFacings
// (a Two Towers facing array) when given.
std::uint32_t convertFacings(Bytes& image, const Bytes& rom, std::uint32_t entry, std::uint32_t ttFacings)
{
    Bytes facings;
    for (int facing = 0; facing < kFacings; ++facing) {
        BoxPlan plan;
        if (ttFacings) plan.tt = ttBoxes(bus_read_u32(ttFacings + facing * 4u), 1);
        const Converted c = convert(rom, entry + facing * kRecordSize, plan);
        Bytes record;
        put32(record, append(image, c.tiles));
        put32(record, append(image, c.frameSet));
        put32(facings, append(image, record));
    }
    return append(image, facings);
}

std::array<int, 3> rgb(const Bytes& rom, std::uint32_t palette, int index)
{
    const std::uint32_t c = r16(rom, palette + index * 2);
    return {static_cast<int>(c & 31), static_cast<int>(c >> 5 & 31), static_cast<int>(c >> 10 & 31)};
}

// Each color of one ROTK palette as the nearest color of another (0 stays clear).
std::array<std::uint8_t, 16> nearestColors(const Bytes& rom, std::uint32_t from, std::uint32_t to)
{
    std::array<std::uint8_t, 16> map = {};
    for (int c = 1; c < 16; ++c) {
        const auto want = rgb(rom, from, c);
        int best = 1, bestDistance = 1 << 30;
        for (int t = 1; t < 16; ++t) {
            const auto have = rgb(rom, to, t);
            const int d = (want[0] - have[0]) * (want[0] - have[0]) + (want[1] - have[1]) * (want[1] - have[1]) +
                          (want[2] - have[2]) * (want[2] - have[2]);
            if (d < bestDistance) best = t, bestDistance = d;
        }
        map[c] = static_cast<std::uint8_t>(best);
    }
    return map;
}

// A transparent animation (one clear 8x8 piece per frame), a frame per box, each with its box.
Converted blankAnimation(const std::vector<Box>& boxes)
{
    Converted out;
    out.tiles.assign(32, 0);
    const std::uint32_t count = static_cast<std::uint32_t>(boxes.size());
    out.frameSet = {8, 8, 0, 0, 0, 0};
    put16(out.frameSet, count);
    put16(out.frameSet, 32);
    out.frameSet.push_back(0);
    out.frameSet.push_back(1);
    const std::uint32_t frameBytes = 6 + 8 + 12;
    for (std::uint32_t f = 0; f < count; ++f) put16(out.frameSet, 2 * count + f * frameBytes);
    for (const Box& box : boxes) {
        const Bytes head = {1, 0, 8, 8, 0, 0};
        const Bytes piece = {0, 0, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0};
        out.frameSet.insert(out.frameSet.end(), head.begin(), head.end());
        out.frameSet.insert(out.frameSet.end(), box.begin(), box.end());
        out.frameSet.insert(out.frameSet.end(), piece.begin(), piece.end());
    }
    return out;
}

// Boxes for a blank animation: at least kBlankFrames frames, the extra ones without a box.
std::vector<Box> blankBoxes(std::vector<Box> boxes)
{
    if (boxes.size() < kBlankFrames) boxes.resize(kBlankFrames, Box{});
    return boxes;
}

void build()
{
    const Bytes& rom = g_state.rotk;
    Bytes& image = g_state.image;
    image.clear();
    put32(image, kMagic);
    put32(image, 0);  // the image's hash, set when it is complete
    Bytes poseTable(kHeroes * kPoses * 4, 0), palettes;
    for (int hero = 0; hero < kHeroes; ++hero) {
        for (int pose = 0; pose < kPoses; ++pose) {
            const Source& source = kSources[pose];
            const std::uint32_t entry = r32(rom, r32(rom, source.banks + hero * 4u) + source.pose * 4u);
            Bytes facings;
            const std::uint32_t ttFacings = bus_read_u32(kPoseTable + (hero * kPoses + pose) * 4u);
            for (int facing = 0; facing < kFacings; ++facing) {
                BoxPlan plan;
                if (pose == kAttack) plan.tt = ttBoxes(bus_read_u32(ttFacings + facing * 4u), 1);
                const Converted c = convert(rom, entry + facing * kRecordSize, plan);
                Bytes record;
                put32(record, append(image, c.tiles));
                put32(record, append(image, c.frameSet));
                put32(facings, append(image, record));
            }
            const std::uint32_t array = append(image, facings);
            std::memcpy(poseTable.data() + (hero * kPoses + pose) * 4, &array, 4);
        }
        const std::uint32_t palette = r32(rom, kObjPalettes + (kHeroPaletteBase + hero) * 4u);
        // the weapon kinds drawn with this hero, as many frames as the body has in each pose
        const auto remap = nearestColors(rom, r32(rom, kObjPalettes + kWeaponsPalette * 4u), palette);
        for (int kind = 0; kind < static_cast<int>(std::size(kWeaponSources)); ++kind) {
            if (kWeaponSources[kind].hero != hero) continue;
            for (int pose = 0; pose < kPoses; ++pose) {
                const Source& source = kSources[pose];
                const std::uint32_t bank = r32(rom, source.banks + hero * 4u);
                const std::uint32_t body = r32(rom, bank + source.pose * 4u);
                const std::uint32_t part = kWeaponSources[kind].part;
                const std::uint32_t entry = part ? r32(rom, bank + (part * 7u + source.pose) * 4u) : 0;
                const bool art = entry >= kRomBase && entry < kRomBase + rom.size();
                const std::uint32_t ttFacings = bus_read_u32(kWeaponPoseTable + (kind * kPoses + pose) * 4u);
                Bytes facings;
                for (int facing = 0; facing < kFacings; ++facing) {
                    const std::uint32_t bodyFrames = r16(rom, r32(rom, body + facing * kRecordSize + 4) + 6);
                    BoxPlan plan;
                    plan.weapon = true;
                    plan.tt = ttBoxes(bus_read_u32(ttFacings + facing * 4u), 0);
                    const Converted c = art ? convert(rom, entry + facing * kRecordSize, plan, remap.data(), bodyFrames)
                                            : blankAnimation(blankBoxes(scaleBoxes(plan.tt, bodyFrames)));
                    Bytes record;
                    put32(record, append(image, c.tiles));
                    put32(record, append(image, c.frameSet));
                    put32(facings, append(image, record));
                }
                g_state.weaponTable[kind * kPoses + pose] = append(image, facings);
            }
        }
        Bytes colors;
        for (std::uint32_t i = 0; i < 32; ++i) colors.push_back(static_cast<std::uint8_t>(r8(rom, palette + i)));
        put32(palettes, append(image, colors));
    }
    // the blank weapon, for kinds the table leaves empty
    const Converted blank = blankAnimation(std::vector<Box>(kBlankFrames));
    Bytes blankRecord, blankFacings;
    put32(blankRecord, append(image, blank.tiles));
    put32(blankRecord, append(image, blank.frameSet));
    const std::uint32_t record = append(image, blankRecord);
    for (int facing = 0; facing < kFacings; ++facing) put32(blankFacings, record);
    g_state.blankWeapon = append(image, blankFacings);
    // the specials, and every code literal that loads one
    g_state.literals.clear();
    for (const Special& special : kSpecials) {
        const std::uint32_t bank = r32(rom, special.source.banks + special.hero * 4u);
        const std::uint32_t array = convertFacings(image, rom, r32(rom, bank + special.source.pose * 4u), special.array);
        for (std::uint32_t a = kCodeStart; a < kCodeEnd; a += 4)
            if (bus_read_u32(a) == special.array) {
                g_state.literals.emplace_back(a, array);
                g_state.literalsLow = std::min(g_state.literalsLow, a);
                g_state.literalsHigh = std::max(g_state.literalsHigh, a);
            }
    }
    g_state.poseTable = append(image, poseTable);
    g_state.palettes = append(image, palettes);
    // FNV-1a of the image after its stamp, so a save state holding another build's image is replaced
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 8; i < image.size(); ++i) hash = (hash ^ image[i]) * 16777619u;
    std::memcpy(image.data() + 4, &hash, 4);
    std::fprintf(stderr, "[ttt] ROTK hero sprites: %zu KiB\n", image.size() / 1024);
}

// The image in host RAM, rewritten when the boot, a reset or a save state replaced it.
void ensureWritten()
{
    std::uint32_t hash;
    std::memcpy(&hash, g_state.image.data() + 4, 4);
    if (bus_read_u32(kDataBase) == kMagic && bus_read_u32(kDataBase + 4) == hash) return;
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
    if (address < kCodeEnd) {
        if (address < g_state.literalsLow || address > g_state.literalsHigh) return 0;
        for (const auto& [literal, array] : g_state.literals)
            if (literal == address) {
                ensureWritten();
                *out = array;
                return 1;
            }
        return 0;
    }
    if (address >= kWeaponPoseTable && address < kWeaponPoseTable + kWeaponEntries * 4u) {
        ensureWritten();
        const std::uint32_t weapon = g_state.weaponTable[(address - kWeaponPoseTable) / 4u];
        // ROTK_TTT_TRACE_WEAPON=1 logs each weapon kind and pose the game first reads
        static std::uint32_t seen[kWeaponEntries] = {};
        if (std::getenv("ROTK_TTT_TRACE_WEAPON") && !seen[(address - kWeaponPoseTable) / 4u]++)
            std::fprintf(stderr, "[weapon] kind %u pose %u\n", (address - kWeaponPoseTable) / 4u / kPoses,
                         (address - kWeaponPoseTable) / 4u % kPoses);
        *out = weapon ? weapon : g_state.blankWeapon;
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
