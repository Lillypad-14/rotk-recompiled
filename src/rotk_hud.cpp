#include "rotk_hud.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "gba_bus.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace rotk {
namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t kRomBase = 0x08000000u;

// Both games draw their HUD with gfx_loadBgAsset(layer, asset, tile base, mode, x, y) on a text BG
// with its tiles at VRAM 0xC000 and its map at 0xE800 (ROTK BG0, The Two Towers BG3, both 0x?D0C).
// A BG asset (ROTK include/gfx.h): u8 flags, u8 flags2, then the parts the flags select: a 256- (bit
// 0) or 16-color (bit 1) palette, a raw block (bit 2), a map (bit 3: u16 width, u16 height, u16
// entries padded to even), tiles (bit 4: u16 size, raw 4bpp when bits 5-6 are 0). Map entries count
// tiles from the asset's own first tile; the loader adds the tile base.
constexpr std::uint32_t kTiles = 0x0600C000u, kMap = 0x0600E800u, kBgPalette = 0x05000000u;

// ROTK (stock ROM): player_unknown08028824 lays the HUD out, player_tickHud's helpers redraw the
// bars from fill tables (gfx level 0x08029948: (steps + 1) * current / maximum, capped).
constexpr std::uint32_t kRotkHudPalette = 0x08781CD4u;  // the middle strip, carrying the palette
struct Piece {
    std::uint32_t asset;  // or a fill table
    std::uint32_t x, y;   // in tiles
    std::uint32_t slot;   // our tile base in The Two Towers' HUD tiles
};
constexpr Piece kMiddle = {0x08781CD4u, 11, 18, 0x01};   // 8x1, 4 tiles
constexpr Piece kMessage = {0x08781D8Cu, 3, 19, 0x05};   // 24x1, 10 tiles
constexpr Piece kCircle = {0x0877F9ECu, 27, 17, 0x50};   // 3x3, 9 tiles: the skill frame
// fill tables of tiles-only assets; the map is laid once from the bar's own asset (as ROTK's setup)
constexpr Piece kHealth = {0x08282998u, 3, 18, 0x0F};    // 31 steps, 8x1, 8 tiles
constexpr Piece kSpirit = {0x08282A14u, 19, 18, 0x17};   // 31 steps, 8x1, 8 tiles
constexpr Piece kVial = {0x08282ACCu, 0, 16, 0x3C};      // 30 steps, 3x4, 12 tiles: experience
constexpr std::uint32_t kHealthMap = 0x0877ABC0u, kSpiritMap = 0x0877CB50u, kVialMap = 0x0877DE88u;
constexpr std::uint32_t kBlankSlot = 0x1F;               // an all-clear tile
constexpr std::uint32_t kHealthSteps = 30, kSpiritSteps = 30, kVialSteps = 29;
// the HUD rows The Two Towers drew on, cleared to kBlankSlot
constexpr std::uint32_t kFirstRow = 16, kLastRow = 19;

// The Two Towers: players at 0x03003640 (0x128 apart; +0x04 experience into the level, +0x08 health,
// +0x0A spirit, +0x16 level, +0x6C/+0x6E their maximums), the local player's index at 0x03003641,
// level thresholds at 0x08050348. g_PlayerHuds 0x030038B0 (0x14 apart): +0x0C redraw flags, 0x10
// the orbs (0x08020D00), 0x08 the experience bar (0x08020E24); 0x080214B4 ticks a player's HUD
// during play. 0x0800DE14 draws the HUD's message line (from 0x08021388, x 5, y 18).
constexpr std::uint32_t kPlayers = 0x03003640u, kPlayerSize = 0x128u, kThresholds = 0x08050348u;
constexpr std::uint32_t kHuds = 0x030038B0u, kHudSize = 0x14u;
constexpr std::uint32_t kOrbsRedraw = 0x08020D00u, kXpRedraw = 0x08020E24u, kHudTick = 0x080214B4u;
constexpr std::uint32_t kHudText = 0x0800DE14u, kHudTextCallerFrom = 0x08021388u, kHudTextCallerTo = 0x080214B4u;
constexpr std::uint32_t kAfterOamCopy = 0x0800E270u;
constexpr std::uint32_t kHudSetup = 0x080208A8u;
// game flags: 0x400000 while the world is parked under a menu (the game's HUD draws then skip)
constexpr std::uint32_t kGameFlags = 0x030039A8u, kWorldParked = 0x00400000u;
// g_PaletteBanks[0]: the BG palette's shadow buffer (copied to palette RAM by fades and cycles)
constexpr std::uint32_t kBgPaletteBank = 0x030003E8u;
constexpr std::uint32_t kPaletteFadeCount = 0x03003620u;  // fades running (they write palette RAM)  // lays a player's HUD out (level start, after menus)
constexpr const char* kHookIds[] = {"ttt.rotk-hud-orbs", "ttt.rotk-hud-xp", "ttt.rotk-hud-tick", "ttt.rotk-hud-text",
                                    "ttt.rotk-hud-vblank", "ttt.rotk-hud-setup"};

struct Asset {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint16_t> map;
    Bytes tiles;
    std::uint16_t palette[16] = {};
    bool hasPalette = false;
};

struct State {
    bool enabled = false;
    Bytes rom;
    Asset middle, message, circle, healthMap, spiritMap, vialMap;
    std::vector<Asset> health, spirit, vial;
    std::uint16_t palette[16] = {};
    int bank = -1;                    // The Two Towers' HUD palette bank, read from its map
    std::uint32_t lastTick = 0;       // VBlanks since the HUD last ticked
    bool drawnStatic = false;  // the palette and bank written since the game last laid its HUD out
    int lastHealth = -1, lastSpirit = -1, lastVial = -1;
} g_state;

std::uint32_t r8(std::uint32_t a) { return a - kRomBase < g_state.rom.size() ? g_state.rom[a - kRomBase] : 0u; }
std::uint32_t r16(std::uint32_t a) { return r8(a) | r8(a + 1) << 8; }
std::uint32_t r32(std::uint32_t a) { return r16(a) | r16(a + 2) << 16; }

Asset parse(std::uint32_t a)
{
    Asset out;
    const std::uint32_t flags = r8(a), flags2 = r8(a + 1);
    std::uint32_t p = a + 2;
    if (flags & 1) p += 512;
    else if (flags & 2) {
        for (int i = 0; i < 16; ++i) out.palette[i] = static_cast<std::uint16_t>(r16(p + 2 * i));
        out.hasPalette = true;
        p += 32;
    }
    if (flags & 4) {
        p += 2 + r16(p);
        p += p & 1;
    }
    if (flags & 8) {
        out.w = r16(p);
        out.h = r16(p + 2);
        p += 4;
        const std::uint32_t n = out.w * out.h;
        for (std::uint32_t i = 0; i < n; ++i)
            out.map.push_back(static_cast<std::uint16_t>(flags2 & 1 ? r8(p + i) : r16(p + 2 * i)));
        p += flags2 & 1 ? n + (n & 1) : 2 * (n + (n & 1));
    }
    if (flags & 0x10) {
        const std::uint32_t n = r16(p);
        for (std::uint32_t i = 0; i < n; ++i) out.tiles.push_back(static_cast<std::uint8_t>(r8(p + 2 + i)));
    }
    return out;
}

std::vector<Asset> table(std::uint32_t at, std::uint32_t steps)
{
    std::vector<Asset> out;
    for (std::uint32_t i = 0; i <= steps; ++i) out.push_back(parse(r32(at + i * 4)));
    return out;
}

// gfx level: (steps + 1) * current / maximum, 0 when empty, capped at steps
int fill(int current, int maximum, int steps)
{
    if (current <= 0 || maximum <= 0) return 0;
    return std::min(steps, (steps + 1) * current / maximum);
}

void writeTiles(const Asset& asset, std::uint32_t slot)
{
    for (std::size_t i = 0; i + 3 < asset.tiles.size(); i += 4) {
        std::uint32_t v;
        std::memcpy(&v, asset.tiles.data() + i, 4);
        bus_write_u32(kTiles + slot * 32 + static_cast<std::uint32_t>(i), v);
    }
}

void writeMap(const Asset& asset, const Piece& at)
{
    for (std::uint32_t y = 0; y < asset.h; ++y)
        for (std::uint32_t x = 0; x < asset.w; ++x) {
            const std::uint16_t e = asset.map[y * asset.w + x];
            const std::uint32_t entry = ((e & 0x3FFu) + at.slot) | (e & 0x0C00u) | static_cast<std::uint32_t>(g_state.bank) << 12;
            bus_write_u16(kMap + ((at.y + y) * 32 + at.x + x) * 2, static_cast<std::uint16_t>(entry));
        }
}

// The Two Towers' HUD palette bank: the most common bank among its HUD rows' entries.
int hudBank()
{
    int counts[16] = {};
    for (std::uint32_t y = kFirstRow + 2; y <= kLastRow; ++y)
        for (std::uint32_t x = 0; x < 30; ++x) {
            const std::uint32_t e = bus_read_u16(kMap + (y * 32 + x) * 2);
            if (e & 0x3FFu) ++counts[e >> 12];
        }
    return static_cast<int>(std::max_element(std::begin(counts), std::end(counts)) - std::begin(counts));
}

void draw()
{
    const std::uint32_t player = bus_read_u8(kPlayers + 1);
    const std::uint32_t p = kPlayers + player * kPlayerSize;
    if (!g_state.drawnStatic) {
        if (g_state.bank < 0) g_state.bank = hudBank();

        g_state.lastHealth = g_state.lastSpirit = g_state.lastVial = -1;
        g_state.drawnStatic = true;
    }
    {
        // the palette into the BG shadow buffer and, unless a fade owns palette RAM, palette RAM: the
        // game reloads this bank (its own HUD pieces carry it)
        const std::uint32_t shadow = bus_read_u32(kBgPaletteBank);
        const bool fading = bus_read_u8(kPaletteFadeCount) != 0;
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t at = static_cast<std::uint32_t>(g_state.bank) * 32 + i * 2;
            if (!fading) bus_write_u16(kBgPalette + at, g_state.palette[i]);
            if (shadow >= 0x02000000u && shadow < 0x04000000u) bus_write_u16(shadow + at, g_state.palette[i]);
        }
        // all of it each time it shows: the game's own HUD layout writes the same tiles and rows
        for (std::uint32_t i = 0; i < 32; i += 4) bus_write_u32(kTiles + kBlankSlot * 32 + i, 0);
        for (std::uint32_t y = kFirstRow; y <= kLastRow; ++y)
            for (std::uint32_t x = 0; x < 30; ++x)
                bus_write_u16(kMap + (y * 32 + x) * 2, static_cast<std::uint16_t>(kBlankSlot | static_cast<std::uint32_t>(g_state.bank) << 12));
        writeTiles(g_state.middle, kMiddle.slot);
        writeMap(g_state.middle, kMiddle);
        writeTiles(g_state.message, kMessage.slot);
        writeMap(g_state.message, kMessage);
        writeTiles(g_state.circle, kCircle.slot);
        writeMap(g_state.circle, kCircle);
        writeMap(g_state.healthMap, kHealth);
        writeMap(g_state.spiritMap, kSpirit);
        writeMap(g_state.vialMap, kVial);
    }
    const int health = fill(static_cast<std::int16_t>(bus_read_u16(p + 0x08)),
                            static_cast<std::int16_t>(bus_read_u16(p + 0x6C)), kHealthSteps);
    const int spirit = fill(static_cast<std::int16_t>(bus_read_u16(p + 0x0A)),
                            static_cast<std::int16_t>(bus_read_u16(p + 0x6E)), kSpiritSteps);
    const std::uint32_t threshold = bus_read_u32(kThresholds + bus_read_u8(p + 0x16) * 4u);
    const int vial = fill(static_cast<int>(std::min<std::uint32_t>(bus_read_u32(p + 0x04), 0x7FFFFFFFu)),
                          static_cast<int>(std::min<std::uint32_t>(threshold, 0x7FFFFFFFu)), kVialSteps);
    writeTiles(g_state.health[health], kHealth.slot);
    writeTiles(g_state.spirit[spirit], kSpirit.slot);
    writeTiles(g_state.vial[vial], kVial.slot);
    if (std::getenv("ROTK_TTT_TRACE_HUD") && (health != g_state.lastHealth || spirit != g_state.lastSpirit || vial != g_state.lastVial))
        std::fprintf(stderr, "[hud] hp %d/%d -> %d, spirit %d/%d -> %d, xp %u/%u -> %d, bank %d, hp tiles %zu map %ux%u\n",
                     static_cast<std::int16_t>(bus_read_u16(p + 0x08)), static_cast<std::int16_t>(bus_read_u16(p + 0x6C)), health,
                     static_cast<std::int16_t>(bus_read_u16(p + 0x0A)), static_cast<std::int16_t>(bus_read_u16(p + 0x6E)), spirit,
                     bus_read_u32(p + 0x04), threshold, vial, g_state.bank, g_state.health[health].tiles.size(),
                     g_state.health[health].w, g_state.health[health].h);
    g_state.lastHealth = health;
    g_state.lastSpirit = spirit;
    g_state.lastVial = vial;
}

int returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    return 1;
}

// The Two Towers' orb and experience redraws: skipped, their redraw flag cleared as they do.
int orbsEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    g_state.lastTick = 0;
    const std::uint32_t flags = kHuds + cpu->R[0] * kHudSize + 0x0C;
    bus_write_u8(flags, bus_read_u8(flags) & ~0x10u);
    return returnToCaller(cpu);
}

int xpEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    g_state.lastTick = 0;
    const std::uint32_t flags = kHuds + cpu->R[0] * kHudSize + 0x0C;
    bus_write_u8(flags, bus_read_u8(flags) & ~0x08u);
    return returnToCaller(cpu);
}

int tickEntry(std::uint32_t, int, ArmCpuState*)
{
    g_state.lastTick = 0;
    return 0;
}

// The game lays its HUD out again (a level, a menu closed): ROTK's goes back over it next VBlank.
int setupEntry(std::uint32_t, int, ArmCpuState*)
{
    g_state.drawnStatic = false;
    g_state.bank = -1;
    g_state.lastTick = 0;
    return 0;
}

// The message line moves to ROTK's message strip, one row down (y is the 6th argument, [sp + 4]).
int textEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t caller = (cpu->R[14] & ~1u) - 4;
    if (caller >= kHudTextCallerFrom && caller < kHudTextCallerTo && bus_read_u32(cpu->R[13] + 4) == 18)
        bus_write_u32(cpu->R[13] + 4, 19);
    return 0;
}

int vblankEntry(std::uint32_t, int, ArmCpuState*)
{
    // the HUD ticks with the game; menus, dialogs and scene changes stop it
    if (g_state.lastTick++ < 4 && !(bus_read_u32(kGameFlags) & kWorldParked)) draw();
    return 0;
}

void reset()
{
    g_state.drawnStatic = false;
    g_state.bank = -1;
    g_state.lastTick = 1000;
    for (const char* id : kHookIds) gba_mod_set_function_hook_enabled(id, 1);
}

}  // namespace

void rotkHudRegister()
{
    const char* on = std::getenv("ROTK_TTT_ROTK_HUD");
    const char* path = std::getenv("ROTK_TTT_ROTK_ROM");
    if (!on || std::strcmp(on, "1") != 0 || !path) return;
    std::ifstream file(path, std::ios::binary);
    g_state.rom.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (g_state.rom.size() < 0x00400000u) {
        std::fprintf(stderr, "[ttt] ROTK HUD: no ROTK ROM at %s\n", path);
        return;
    }
    g_state.middle = parse(kMiddle.asset);
    g_state.message = parse(kMessage.asset);
    g_state.circle = parse(kCircle.asset);
    g_state.healthMap = parse(kHealthMap);
    g_state.spiritMap = parse(kSpiritMap);
    g_state.vialMap = parse(kVialMap);
    g_state.health = table(kHealth.asset, kHealthSteps);
    g_state.spirit = table(kSpirit.asset, kSpiritSteps);
    g_state.vial = table(kVial.asset, kVialSteps);
    std::memcpy(g_state.palette, parse(kRotkHudPalette).palette, sizeof g_state.palette);
    g_state.enabled = true;
    gba_mod_register_function_entry_plugin(kHookIds[0], kOrbsRedraw, 1, orbsEntry);
    gba_mod_register_function_entry_plugin(kHookIds[1], kXpRedraw, 1, xpEntry);
    gba_mod_register_function_entry_plugin(kHookIds[2], kHudTick, 1, tickEntry);
    gba_mod_register_function_entry_plugin(kHookIds[3], kHudText, 1, textEntry);
    gba_mod_register_function_entry_plugin(kHookIds[4], kAfterOamCopy, 1, vblankEntry);
    gba_mod_register_function_entry_plugin(kHookIds[5], kHudSetup, 1, setupEntry);
    gba_mod_register_reset_callback(reset);
    std::fprintf(stderr, "[ttt] ROTK HUD: %zu + %zu + %zu bar steps\n", g_state.health.size(), g_state.spirit.size(),
                 g_state.vial.size());
}

}  // namespace rotk
