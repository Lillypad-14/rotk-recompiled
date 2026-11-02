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
#include "gba_ppu.h"
#include "mod_function_hooks.h"
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
// fists and the hat are part of ROTK's body art. The Two Towers draws the weapon in the hero's
// OBJ bank; these keep ROTK's weapons palette (bank 2) through the renderer's palette provider.
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
// Drawing: sprite_composeOam(actor) pushes an actor's pieces through sprite_pushOam(index, oam);
// 0x08032708 pushes one piece of no actor. The VBlank handler copies the OAM shadow and then calls
// 0x0800E270 (its only caller). Actor+0x24 is the current {tiles, frame set} record.
constexpr std::uint32_t kComposeOam = 0x08001EB8u, kPushOam = 0x0802FA68u, kPushLoose = 0x08032708u;
constexpr std::uint32_t kAfterOamCopy = 0x0800E270u;
constexpr std::uint32_t kActorRecord = 0x24u;
constexpr std::uint32_t kOamEntries = 128;
constexpr const char* kHookIds[] = {"ttt.rotk-sprites-compose", "ttt.rotk-sprites-push", "ttt.rotk-sprites-loose",
                                    "ttt.rotk-sprites-oam-copy"};
enum Pose { kWalk, kStand, kAttack, kDeath, kFlinch, kCast };
// ROTK's combo: an attack that starts soon after the last one plays the next swing (melee poses 0,
// 2 and 4). Looks only: the Two Towers' attack timing and damage are unchanged. The game reads the
// body's attack entry once when an attack starts and the weapon's on every tick. Times are the
// game's frame counter (60 a second): word +0x1C of the struct at *0x03003910.
constexpr int kComboSteps = 3;
constexpr std::uint32_t kComboSwings[kComboSteps] = {0, 2, 4};
constexpr std::uint32_t kGameStatePointer = 0x03003910u, kGameFrame = 0x1Cu;
// within kComboWindow frames of the last attack's start, the next swing (stock attacks start at
// least 20 apart); within kComboSame, the same attack read again
constexpr std::uint32_t kComboWindow = 50, kComboSame = 8;

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

// Dialog portraits. The Two Towers: DialogLineInfos-like table (8 bytes per text line, the
// speaker's BG asset first; scene_inGameDialog 0x08013498 loads it, a 6x6-tile map). Its
// portraits are 16-color, LZ77 (flags 0x52), a gold ring drawn into the art.
constexpr std::uint32_t kLinePortraits = 0x0832B524u, kLinePortraitCount = 1436;
// ROTK: 7x8 tiles, 16 colors, ROTK's pucrunch (flags 0x72): the active hero's, per hero, are code
// literals in scene_inGameDialog_enter 0x0801AB44 + 8 * hero.
constexpr std::uint32_t kRotkHeroPortraits = 0x0801AB44u;
constexpr int kRotkPortraitWide = 7, kPortraitTiles = 6;
constexpr int kPortraitCropX = 4, kPortraitCropY = 4;  // ROTK's face in a 48x48 square
struct PortraitSwap {
    std::uint32_t tt;  // a Two Towers hero portrait
    int hero;
};
constexpr PortraitSwap kPortraitSwaps[] = {
    {0x08CA97A8u, 0},                     // Frodo
    {0x08CA8AA8u, 1},                     // Legolas
    {0x08CA8690u, 2},                     // Aragorn
    {0x08CAA444u, 3}, {0x08CAA874u, 3},   // Gandalf the Grey, the White
    {0x08CAB018u, 4},                     // Eowyn
    {0x08CA8EFCu, 5},                     // Gimli
};

// Converted data in host RAM, past the plain host's own use of its first page.
constexpr std::uint32_t kDataBase = 0x12100000u;
constexpr std::uint32_t kMagic = 0x53544B52u;  // "RKTS"

struct State {
    bool enabled = false;
    Bytes rotk;
    Bytes image;  // written at kDataBase
    std::uint32_t poseTable = 0, palettes = 0, blankWeapon = 0;
    std::uint32_t portraits[std::size(kPortraitSwaps)] = {};
    std::uint32_t weaponTable[kWeaponEntries] = {};
    // the attack's facing arrays per combo step (step 0 is the tables' own)
    std::uint32_t comboBody[kHeroes][kComboSteps] = {};
    std::uint32_t comboWeapon[kWeaponEntries / kPoses][kComboSteps] = {};
    // per hero: the combo step showing and when its attack started
    int comboStep[kHeroes] = {};
    std::uint32_t comboStart[kHeroes] = {};
    bool comboStarted[kHeroes] = {};
    // code literals that load a special animation, and the converted facing array each answers with
    std::vector<std::pair<std::uint32_t, std::uint32_t>> literals;
    std::uint32_t literalsLow = ~0u, literalsHigh = 0;
    // the records of weapons with ROTK art (sorted), drawn in weaponColors
    std::vector<std::uint32_t> weaponRecords;
    // the idle records (sorted) with ROTK's ticks per frame: the Two Towers idles at 3, ROTK at 2
    std::vector<std::pair<std::uint32_t, std::uint8_t>> idleTicks;
    std::array<std::uint8_t, 32> weaponColors = {};
    // per OAM index: drawn in weaponColors, as pushed and as last copied to OAM
    bool actorWeapon = false;
    std::array<bool, kOamEntries> pending = {}, displayed = {};
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
// planned and at least minFrames frames (the last repeated).
Converted convert(const Bytes& rom, std::uint32_t animation, const BoxPlan& plan, std::uint32_t minFrames = 0)
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

// ROTK's pucrunch stream (rotkit/compression/pucrunch.py): a 4-byte header, the RLE byte table,
// then MSB-first bits in little-endian words.
Bytes unpackRotk(const Bytes& rom, std::uint32_t at)
{
    const std::uint32_t tableSize = r8(rom, at), extraBits = r8(rom, at + 2), escBits = r8(rom, at + 3);
    std::uint32_t escape = r8(rom, at + 1), pos = at + 4 + tableSize, word = 0, left = 0;
    auto bits = [&](std::uint32_t n) {
        std::uint32_t value = 0;
        while (n) {
            if (!left) {
                word = r32(rom, pos);
                pos += 4;
                left = 32;
            }
            const std::uint32_t take = std::min(n, left);
            left -= take;
            value = value << take | (word >> left & ((1u << take) - 1));
            n -= take;
        }
        return value;
    };
    auto gamma = [&]() {
        std::uint32_t k = 0;
        while (k < 7 && bits(1)) ++k;
        return 1u << k | bits(k);
    };
    Bytes out;
    auto copy = [&](std::uint32_t dist, std::uint32_t length) {
        for (std::uint32_t i = 0; i < length; ++i) out.push_back(dist <= out.size() ? out[out.size() - dist] : 0);
    };
    const std::uint32_t lowBits = 8 - escBits;
    while (out.size() < 0x10000) {
        const std::uint32_t top = bits(escBits);
        if (top != escape) {
            out.push_back(static_cast<std::uint8_t>(top << lowBits | bits(lowBits)));
            continue;
        }
        const std::uint32_t length = gamma() + 1;
        if (length > 2) {
            std::uint32_t hi = gamma() - 1;
            if (hi == 254) break;
            hi = hi << extraBits | bits(extraBits);
            copy((hi << 8 | bits(8)) + 1, length);
        } else if (!bits(1)) {
            copy(bits(8) + 1, 2);
        } else if (!bits(1)) {
            const std::uint32_t next = bits(escBits);
            out.push_back(static_cast<std::uint8_t>(escape << lowBits | bits(lowBits)));
            escape = next;
        } else {
            std::uint32_t count = gamma();
            if (count >= 128) {
                count = (count << 1 | bits(1)) & 0xFF;
                count |= (gamma() - 1) << 8;
            }
            const std::uint32_t code = gamma();
            const std::uint8_t value = static_cast<std::uint8_t>(
                code < 32 ? r8(rom, at + 4 + code - 1) : (code << 3 | bits(3)) & 0xFF);
            out.insert(out.end(), count + 1, value);
        }
    }
    return out;
}

// A hero's ROTK dialog portrait as a Two Towers one: its palette and 6x6 raw tiles (flags 0x12),
// cropped from ROTK's 7x8.
Bytes convertPortrait(const Bytes& rom, int hero)
{
    const std::uint32_t asset = r32(rom, kRotkHeroPortraits + 8u * hero);
    const Bytes tiles = unpackRotk(rom, asset + 2 + 32 + 2);
    auto pixel = [&](int x, int y) {
        const std::size_t at = static_cast<std::size_t>((y / 8 * kRotkPortraitWide + x / 8) * 32 + y % 8 * 4 + x % 8 / 2);
        return at < tiles.size() ? (tiles[at] >> (x % 2 * 4) & 15) : 0;
    };
    Bytes out = {0x12, 0x00};
    for (std::uint32_t i = 0; i < 32; ++i) out.push_back(static_cast<std::uint8_t>(r8(rom, asset + 2 + i)));
    put16(out, kPortraitTiles * kPortraitTiles * 32);
    for (int t = 0; t < kPortraitTiles * kPortraitTiles; ++t)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; x += 2) {
                const int px = kPortraitCropX + t % kPortraitTiles * 8 + x, py = kPortraitCropY + t / kPortraitTiles * 8 + y;
                out.push_back(static_cast<std::uint8_t>(pixel(px, py) | pixel(px + 1, py) << 4));
            }
    return out;
}

void build()
{
    const Bytes& rom = g_state.rotk;
    Bytes& image = g_state.image;
    image.clear();
    g_state.weaponRecords.clear();
    g_state.idleTicks.clear();
    const std::uint32_t weaponsPalette = r32(rom, kObjPalettes + kWeaponsPalette * 4u);
    for (std::uint32_t i = 0; i < 32; ++i) g_state.weaponColors[i] = static_cast<std::uint8_t>(r8(rom, weaponsPalette + i));
    put32(image, kMagic);
    put32(image, 0);  // the image's hash, set when it is complete
    Bytes poseTable(kHeroes * kPoses * 4, 0), palettes;
    // a hero's body animation as a facing array; attack boxes from the Two Towers' pose when attacking
    auto bodyArray = [&](int hero, const Source& source, int pose) {
        const std::uint32_t entry = r32(rom, r32(rom, source.banks + hero * 4u) + source.pose * 4u);
        const std::uint32_t ttFacings = bus_read_u32(kPoseTable + (hero * kPoses + pose) * 4u);
        Bytes facings;
        for (int facing = 0; facing < kFacings; ++facing) {
            BoxPlan plan;
            if (pose == kAttack) plan.tt = ttBoxes(bus_read_u32(ttFacings + facing * 4u), 1);
            const Converted c = convert(rom, entry + facing * kRecordSize, plan);
            Bytes record;
            put32(record, append(image, c.tiles));
            put32(record, append(image, c.frameSet));
            const std::uint32_t at = append(image, record);
            if (pose == kStand) g_state.idleTicks.emplace_back(at, r8(rom, entry + facing * kRecordSize + 12));
            put32(facings, at);
        }
        return append(image, facings);
    };
    // a weapon kind's animation drawn with its hero's body, as many frames as the body has
    auto weaponArray = [&](int kind, const Source& source, int pose) {
        const int hero = kWeaponSources[kind].hero;
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
            const Converted c = art ? convert(rom, entry + facing * kRecordSize, plan, bodyFrames)
                                    : blankAnimation(blankBoxes(scaleBoxes(plan.tt, bodyFrames)));
            Bytes record;
            put32(record, append(image, c.tiles));
            put32(record, append(image, c.frameSet));
            const std::uint32_t at = append(image, record);
            if (art) g_state.weaponRecords.push_back(at);
            put32(facings, at);
        }
        return append(image, facings);
    };
    for (int hero = 0; hero < kHeroes; ++hero) {
        for (int pose = 0; pose < kPoses; ++pose) {
            const std::uint32_t array = bodyArray(hero, kSources[pose], pose);
            std::memcpy(poseTable.data() + (hero * kPoses + pose) * 4, &array, 4);
            if (pose == kAttack) g_state.comboBody[hero][0] = array;
        }
        for (int step = 1; step < kComboSteps; ++step)
            g_state.comboBody[hero][step] = bodyArray(hero, {kMeleeBanks, kComboSwings[step]}, kAttack);
        const std::uint32_t palette = r32(rom, kObjPalettes + (kHeroPaletteBase + hero) * 4u);
        for (int kind = 0; kind < static_cast<int>(std::size(kWeaponSources)); ++kind) {
            if (kWeaponSources[kind].hero != hero) continue;
            for (int pose = 0; pose < kPoses; ++pose)
                g_state.weaponTable[kind * kPoses + pose] = weaponArray(kind, kSources[pose], pose);
            g_state.comboWeapon[kind][0] = g_state.weaponTable[kind * kPoses + kAttack];
            for (int step = 1; step < kComboSteps; ++step)
                g_state.comboWeapon[kind][step] = weaponArray(kind, {kMeleeBanks, kComboSwings[step]}, kAttack);
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
    for (std::size_t i = 0; i < std::size(kPortraitSwaps); ++i)
        g_state.portraits[i] = append(image, convertPortrait(rom, kPortraitSwaps[i].hero));
    std::sort(g_state.weaponRecords.begin(), g_state.weaponRecords.end());
    std::sort(g_state.idleTicks.begin(), g_state.idleTicks.end());
    g_state.poseTable = append(image, poseTable);
    g_state.palettes = append(image, palettes);
    // FNV-1a of the image after its stamp, so a save state holding another build's image is replaced
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 8; i < image.size(); ++i) hash = (hash ^ image[i]) * 16777619u;
    std::memcpy(image.data() + 4, &hash, 4);
    std::fprintf(stderr, "[ttt] ROTK hero sprites: %zu KiB, %zu special literals\n", image.size() / 1024,
                 g_state.literals.size());
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

// A hero's attack starting: the combo's next swing when the last one started recently.
std::uint32_t comboAttack(int hero)
{
    const std::uint32_t now = bus_read_u32(bus_read_u32(kGameStatePointer) + kGameFrame);
    const std::uint32_t since = now - g_state.comboStart[hero];
    int& step = g_state.comboStep[hero];
    if (!g_state.comboStarted[hero] || since > kComboWindow)
        step = 0;
    else if (since >= kComboSame)
        step = (step + 1) % kComboSteps;
    if (!g_state.comboStarted[hero] || since >= kComboSame) g_state.comboStart[hero] = now;
    g_state.comboStarted[hero] = true;
    return g_state.comboBody[hero][step];
}

int romRead32(std::uint32_t address, std::uint32_t original, std::uint32_t* out)
{
    // built on the first ROM read: the cart is not mapped yet when reset runs. The build reads the
    // Two Towers' own tables, so without the override.
    if (g_state.image.empty()) {
        gba::g_rom_read32_override = nullptr;
        build();
        gba::g_rom_read32_override = romRead32;
    }
    if (address >= kPoseTable && address < kPoseTable + kHeroes * kPoses * 4u) {
        ensureWritten();
        const std::uint32_t entry = (address - kPoseTable) / 4u;
        *out = entry % kPoses == kAttack ? comboAttack(static_cast<int>(entry / kPoses))
                                         : bus_read_u32(g_state.poseTable + entry * 4u);
        return 1;
    }
    if (address < kCodeEnd) {
        if (address < g_state.literalsLow || address > g_state.literalsHigh) return 0;
        for (const auto& [literal, array] : g_state.literals)
            if (literal == address) {
                ensureWritten();
                // ROTK_TTT_TRACE_SPECIAL=1 logs each special read, with the game frame
                if (std::getenv("ROTK_TTT_TRACE_SPECIAL"))
                    std::fprintf(stderr, "[special] %08X at %u\n", address,
                                 bus_read_u32(bus_read_u32(kGameStatePointer) + kGameFrame));
                *out = array;
                return 1;
            }
        return 0;
    }
    if (address >= kWeaponPoseTable && address < kWeaponPoseTable + kWeaponEntries * 4u) {
        ensureWritten();
        const std::uint32_t entry = (address - kWeaponPoseTable) / 4u, kind = entry / kPoses;
        const std::uint32_t weapon = entry % kPoses == kAttack
                                         ? g_state.comboWeapon[kind][g_state.comboStep[kWeaponSources[kind].hero]]
                                         : g_state.weaponTable[entry];
        // ROTK_TTT_TRACE_WEAPON=1 logs each weapon kind and pose the game first reads
        static std::uint32_t seen[kWeaponEntries] = {};
        if (std::getenv("ROTK_TTT_TRACE_WEAPON") && !seen[(address - kWeaponPoseTable) / 4u]++)
            std::fprintf(stderr, "[weapon] kind %u pose %u\n", (address - kWeaponPoseTable) / 4u / kPoses,
                         (address - kWeaponPoseTable) / 4u % kPoses);
        *out = weapon ? weapon : g_state.blankWeapon;
        return 1;
    }
    if (address >= kLinePortraits && address < kLinePortraits + kLinePortraitCount * 8u && address % 8 == kLinePortraits % 8) {
        for (std::size_t i = 0; i < std::size(kPortraitSwaps); ++i)
            if (original == kPortraitSwaps[i].tt) {
                ensureWritten();
                *out = g_state.portraits[i];
                return 1;
            }
        return 0;
    }
    if (address >= kHeroPalettes && address < kHeroPalettes + kHeroes * 4u) {
        ensureWritten();
        *out = bus_read_u32(g_state.palettes + (address - kHeroPalettes));
        return 1;
    }
    return 0;
}

int composeEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t record = bus_read_u32(cpu->R[0] + kActorRecord);
    g_state.actorWeapon = std::binary_search(g_state.weaponRecords.begin(), g_state.weaponRecords.end(), record);
    // idle at ROTK's speed: actor+0x21 is the ticks per frame, +0x20 the countdown (actor_tickAnimation)
    const auto idle = std::lower_bound(g_state.idleTicks.begin(), g_state.idleTicks.end(), std::make_pair(record, std::uint8_t{0}));
    if (idle != g_state.idleTicks.end() && idle->first == record && idle->second) {
        const std::uint32_t actor = cpu->R[0];
        if (bus_read_u8(actor + 0x21) != idle->second) {
            bus_write_u8(actor + 0x21, idle->second);
            if (bus_read_u8(actor + 0x20) > idle->second) bus_write_u8(actor + 0x20, idle->second);
        }
    }
    return 0;
}

int pushEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t index = cpu->R[0] & 0xFFu;
    if (index < kOamEntries) g_state.pending[index] = g_state.actorWeapon;
    return 0;
}

int looseEntry(std::uint32_t, int, ArmCpuState*)
{
    g_state.actorWeapon = false;
    return 0;
}

int oamCopyEntry(std::uint32_t, int, ArmCpuState*)
{
    g_state.displayed = g_state.pending;
    g_state.pending.fill(false);
    // ROTK_TTT_SKILL_TEST=N (tests): player 0 knows and selects skill N, spirit kept full. Player
    // records at 0x03003640, 0x128 apart: +0x0A spirit, +0x6E its maximum, +0x1E the selected
    // skill, skill levels at +0x25 + N and +0x30 + N.
    if (const char* test = std::getenv("ROTK_TTT_SKILL_TEST")) {
        constexpr std::uint32_t kPlayer = 0x03003640u;
        const std::uint32_t skill = static_cast<std::uint32_t>(std::atoi(test));
        bus_write_u8(kPlayer + 0x1E, static_cast<std::uint8_t>(skill));
        if (bus_read_u8(kPlayer + 0x25 + skill) == 0) bus_write_u8(kPlayer + 0x25 + skill, 1);
        if (bus_read_u8(kPlayer + 0x30 + skill) == 0) bus_write_u8(kPlayer + 0x30 + skill, 1);
        bus_write_u16(kPlayer + 0x0A, bus_read_u16(kPlayer + 0x6E));
        // +0x21 bit 1 blocks casting (set in Frodo's and Eowyn's starting levels)
        bus_write_u8(kPlayer + 0x21, bus_read_u8(kPlayer + 0x21) & ~2u);
        // +0x19 bit 3: a shield is worn (Eowyn's Shield Bash needs one)
        if (std::getenv("ROTK_TTT_SKILL_TEST_SHIELD"))
            bus_write_u8(kPlayer + 0x19, bus_read_u8(kPlayer + 0x19) | 8u);
    }
    if (const char* dump = std::getenv("ROTK_TTT_DUMP_PLAYER")) {
        static int frames = 0;
        if (++frames == std::atoi(dump))
            for (std::uint32_t p = 0; p < 2; ++p) {
                std::fprintf(stderr, "[player %u]", p);
                for (std::uint32_t i = 0; i < 0x128; ++i) {
                    if (i % 32 == 0) std::fprintf(stderr, "\n  %03X:", i);
                    std::fprintf(stderr, " %02X", bus_read_u8(0x03003640u + p * 0x128u + i));
                }
                std::fprintf(stderr, "\n");
            }
    }
    return 0;
}

const std::uint8_t* paletteProvider(std::uint32_t index)
{
    return index < kOamEntries && g_state.displayed[index] ? g_state.weaponColors.data() : nullptr;
}

void reset()
{
    gba::g_rom_read32_override = nullptr;
    if (!g_state.enabled) return;
    gba::g_rom_read32_override = romRead32;
    g_state.actorWeapon = false;
    std::fill(std::begin(g_state.comboStep), std::end(g_state.comboStep), 0);
    std::fill(std::begin(g_state.comboStarted), std::end(g_state.comboStarted), false);
    g_state.pending.fill(false);
    g_state.displayed.fill(false);
    gba::g_obj_palette_provider = paletteProvider;
    for (const char* id : kHookIds) gba_mod_set_function_hook_enabled(id, 1);
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
    gba_mod_register_function_entry_plugin(kHookIds[0], kComposeOam, 1, composeEntry);
    gba_mod_register_function_entry_plugin(kHookIds[1], kPushOam, 1, pushEntry);
    gba_mod_register_function_entry_plugin(kHookIds[2], kPushLoose, 1, looseEntry);
    gba_mod_register_function_entry_plugin(kHookIds[3], kAfterOamCopy, 1, oamCopyEntry);
    gba_mod_register_reset_callback(reset);
}

}  // namespace rotk
