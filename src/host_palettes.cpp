#include "host_palettes.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace rotk {
namespace {

// sprite_composeOam(Actor*) pushes an actor's pieces; fx_drawParticle pushes a particle's, which
// have no actor. Native calls do not keep the caller in LR, so a piece's owner is the latest of the
// two entries.
constexpr std::uint32_t kComposeOam = 0x08002484u;
constexpr std::uint32_t kDrawParticle = 0x08039324u;
// sprite_composeFrame(frameSet, frameIndex, flags, origin, tileNumber, oam, actor) pushes one frame's
// pieces. Its tile number (the first stack argument) is where the frame's tiles sit in OBJ VRAM.
constexpr std::uint32_t kComposeFrame = 0x08002cd8u;
constexpr const char* kComposeHookId = "rotk.pc-host-palettes-compose";
constexpr const char* kParticleHookId = "rotk.pc-host-palettes-particle";
constexpr const char* kFrameHookId = "rotk.pc-host-palettes-frame";
// Piece palettes (romhack hacks/tools/hero_data.py): a ROM frame set preceded by "HPAL" and a table
// address. Table: u32 frame count, a u32 entry address per frame (0: none). Entry: u32 piece count,
// then per piece u16 tile offset, u16 1 if it has its own colors, and 16 BGR555 colors.
constexpr std::uint32_t kPieceMagic = 0x4C415048u;  // "HPAL"
constexpr std::uint32_t kRomStart = 0x08000000u;
constexpr std::uint32_t kRomEnd = 0x0A000000u;
constexpr std::uint32_t kPieceBytes = 36u;
// Actor.spriteAnimation, SpriteAnimation.palette. Only live actors are composed, so no freed check
// (spawned effects have type 0xFFFF while alive).
constexpr std::uint32_t kActorAnimation = 0x24u;
constexpr std::uint32_t kAnimationPalette = 0x08u;
// Palettes in host RAM belong to sprites the host converted; no stock or mod sprite has one there.
constexpr std::uint32_t kHostRamStart = 0x12000000u;
constexpr std::uint32_t kHostRamEnd = 0x16000000u;
constexpr std::uint32_t kPaletteBytes = 32u;

using Colors = std::array<std::uint8_t, kPaletteBytes>;

struct State {
    bool enabled = false;
    bool animationPalettes = false;  // converted sprites with palettes in host RAM (Two Towers data)
    std::uint32_t actor = 0;  // owner of the pieces pushed now
    std::uint32_t frameEntry = 0;  // piece palettes of the frame being pushed, or 0
    std::uint32_t frameTile = 0;  // its first OBJ tile
    // Per shadow buffer: each piece's palette (an index into colors, -1 for its OBJ bank).
    std::vector<int> pieces[2];
    std::vector<Colors> colors[2];
    std::vector<std::uint32_t> addresses[2];  // the guest address of each entry in colors
    int displayed = -1;
    std::uint64_t pushed = 0, owned = 0;
    // ROTK_PC_PIECE_PALETTE_LOG=1: composeFrame calls, tagged frame sets, tagged pieces, matched.
    bool log = false;
    std::uint64_t frames = 0, tagged = 0, tries = 0, matched = 0;
};

State g_states[3];  // solo, co-op console 1, co-op console 2 (each has its own sprites)
State* g = &g_states[0];

int composeEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    g->actor = cpu->R[0];
    g->frameEntry = 0;
    return 0;  // observe only
}

int particleEntry(std::uint32_t, int, ArmCpuState*)
{
    g->actor = 0;
    g->frameEntry = 0;
    return 0;
}
int frameEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    g->frameEntry = 0;
    ++g->frames;
    const std::uint32_t frameSet = cpu->R[0];
    if (frameSet < kRomStart + 8u || frameSet >= kRomEnd || bus_read_u32(frameSet - 8u) != kPieceMagic) return 0;
    const std::uint32_t table = bus_read_u32(frameSet - 4u);
    const std::uint32_t index = cpu->R[1] & 0xFFFFu;
    if (table < kRomStart || table >= kRomEnd || index >= bus_read_u32(table)) return 0;
    g->frameEntry = bus_read_u32(table + 4u + index * 4u);
    ++g->tagged;
    g->frameTile = bus_read_u16(cpu->R[13]);
    return 0;
}
// The colors of a pushed piece of the current frame (matched by its tile), or 0.
std::uint32_t piecePalette(std::uint32_t oam)
{
    const std::uint32_t entry = g->frameEntry;
    if (entry < kRomStart || entry >= kRomEnd || oam == 0) return 0;
    const std::uint32_t offset = (bus_read_u16(oam + 4u) - g->frameTile) & 0x3FFu;
    ++g->tries;
    const std::uint32_t count = bus_read_u32(entry);
    for (std::uint32_t k = 0; k < count && k < 128u; ++k) {
        const std::uint32_t piece = entry + 4u + k * kPieceBytes;
        if (bus_read_u16(piece) == offset) {
            ++g->matched;
            return bus_read_u16(piece + 2u) ? piece + 4u : 0;
        }
    }
    return 0;
}

// The host-RAM palette of the actor's current animation, or 0.
std::uint32_t ownPalette(std::uint32_t actor)
{
    if (actor == 0) return 0;
    const std::uint32_t animation = bus_read_u32(actor + kActorAnimation);
    if (animation < 0x02000000u) return 0;
    if (!g->animationPalettes) return 0;
    const std::uint32_t palette = bus_read_u32(animation + kAnimationPalette);
    return palette >= kHostRamStart && palette + kPaletteBytes <= kHostRamEnd ? palette : 0;
}

int colorsIndex(int buffer, std::uint32_t address)
{
    std::vector<std::uint32_t>& addresses = g->addresses[buffer];
    for (std::size_t i = 0; i < addresses.size(); ++i)
        if (addresses[i] == address) return static_cast<int>(i);
    Colors colors;
    for (std::uint32_t i = 0; i < kPaletteBytes; ++i) colors[i] = bus_read_u8(address + i);
    addresses.push_back(address);
    g->colors[buffer].push_back(colors);
    return static_cast<int>(addresses.size() - 1);
}

const std::uint8_t* paletteProvider(std::uint32_t index)
{
    const int buffer = g->displayed;
    if (buffer < 0) return nullptr;
    const std::vector<int>& pieces = g->pieces[buffer];
    if (index >= pieces.size() || pieces[index] < 0) return nullptr;
    return g->colors[buffer][pieces[index]].data();
}

}  // namespace

void hostPalettesRegister()
{
    gba_mod_register_function_entry_plugin(kComposeHookId, kComposeOam, 1, composeEntry);
    gba_mod_register_function_entry_plugin(kParticleHookId, kDrawParticle, 1, particleEntry);
    gba_mod_register_function_entry_plugin(kFrameHookId, kComposeFrame, 1, frameEntry);
}

void hostPalettesApply(bool twoTowersData)
{
    g_states[0] = State{};
    g = &g_states[0];
    g->enabled = true;
    g->animationPalettes = twoTowersData;
    if (const char* v = std::getenv("ROTK_PC_PIECE_PALETTE_LOG")) g->log = v[0] == '1';
    if (g->log) std::fprintf(stderr, "[rotk] piece palettes on\n");
    gba_mod_set_function_hook_enabled(kComposeHookId, g->enabled);
    gba_mod_set_function_hook_enabled(kParticleHookId, g->enabled);
    gba_mod_set_function_hook_enabled(kFrameHookId, g->enabled);
    gba::g_obj_palette_provider = g->enabled ? paletteProvider : nullptr;
}

// Co-op: both consoles compose sprites, so each needs its own owner/frame tracking and piece table,
// and the compose hooks must keep running while the session suspends mod hooks. Without this the
// custom-colored hero anims (Witch-king, Guard of Gondor) drew in the wrong OBJ bank's colors.
void hostPalettesStartCoop()
{
    for (int console = 1; console <= 2; ++console) {
        State& state = g_states[console];
        state = State{};
        state.enabled = g_states[0].enabled;
        state.animationPalettes = g_states[0].animationPalettes;
        state.log = g_states[0].log;
    }
    for (const char* id : {kComposeHookId, kParticleHookId, kFrameHookId})
        gba_mod_set_function_hook_runs_suspended(id, 1);
}

void hostPalettesEndCoop()
{
    for (const char* id : {kComposeHookId, kParticleHookId, kFrameHookId})
        gba_mod_set_function_hook_runs_suspended(id, 0);
    hostPalettesSelectConsole(-1);
}

void hostPalettesSelectConsole(int console)
{
    g = console >= 0 && console < 2 ? &g_states[console + 1] : &g_states[0];
}

bool hostPalettesActive()
{
    return g->enabled;
}
bool hostPalettesConvertedSprites()
{
    return g->enabled && g->animationPalettes;
}
void hostPalettesNotePiece(int buffer, std::uint32_t index, std::uint32_t oam)
{
    if (!g->enabled || buffer < 0 || buffer > 1) return;
    std::vector<int>& pieces = g->pieces[buffer];
    if (index == 0) {
        // a new frame in this buffer
        pieces.clear();
        g->colors[buffer].clear();
        g->addresses[buffer].clear();
    }
    if (pieces.size() <= index) pieces.resize(index + 1u, -1);
    std::uint32_t palette = piecePalette(oam);
    if (!palette) palette = ownPalette(g->actor);
    pieces[index] = palette ? colorsIndex(buffer, palette) : -1;
    ++g->pushed;
    if (palette) ++g->owned;
    if (g->log && g->pushed % 512u == 0)
        std::fprintf(stderr, "[rotk] piece palettes: %llu frames, %llu tagged, %llu tagged pieces, %llu matched, "
                     "%llu of %llu pieces in own colors (last tile %u, frame tile %u)\n",
                     (unsigned long long)g->frames, (unsigned long long)g->tagged,
                     (unsigned long long)g->tries, (unsigned long long)g->matched,
                     (unsigned long long)g->owned, (unsigned long long)g->pushed,
                     oam ? (unsigned)(bus_read_u16(oam + 4u) & 0x3FFu) : 0u, (unsigned)g->frameTile);
}

void hostPalettesSetDisplayed(int buffer)
{
    g->displayed = buffer;
}

}  // namespace rotk
