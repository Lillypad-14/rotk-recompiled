#include "host_palettes.h"

#include <array>
#include <cstdio>
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
constexpr const char* kComposeHookId = "rotk.pc-host-palettes-compose";
constexpr const char* kParticleHookId = "rotk.pc-host-palettes-particle";
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
    std::uint32_t actor = 0;  // owner of the pieces pushed now
    // Per shadow buffer: each piece's palette (an index into colors, -1 for its OBJ bank).
    std::vector<int> pieces[2];
    std::vector<Colors> colors[2];
    std::vector<std::uint32_t> addresses[2];  // the guest address of each entry in colors
    int displayed = -1;
    std::uint64_t pushed = 0, owned = 0;
};

State g_state;

int composeEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    g_state.actor = cpu->R[0];
    return 0;  // observe only
}

int particleEntry(std::uint32_t, int, ArmCpuState*)
{
    g_state.actor = 0;
    return 0;
}

// The host-RAM palette of the actor's current animation, or 0.
std::uint32_t ownPalette(std::uint32_t actor)
{
    if (actor == 0) return 0;
    const std::uint32_t animation = bus_read_u32(actor + kActorAnimation);
    if (animation < 0x02000000u) return 0;
    const std::uint32_t palette = bus_read_u32(animation + kAnimationPalette);
    return palette >= kHostRamStart && palette + kPaletteBytes <= kHostRamEnd ? palette : 0;
}

int colorsIndex(int buffer, std::uint32_t address)
{
    std::vector<std::uint32_t>& addresses = g_state.addresses[buffer];
    for (std::size_t i = 0; i < addresses.size(); ++i)
        if (addresses[i] == address) return static_cast<int>(i);
    Colors colors;
    for (std::uint32_t i = 0; i < kPaletteBytes; ++i) colors[i] = bus_read_u8(address + i);
    addresses.push_back(address);
    g_state.colors[buffer].push_back(colors);
    return static_cast<int>(addresses.size() - 1);
}

const std::uint8_t* paletteProvider(std::uint32_t index)
{
    const int buffer = g_state.displayed;
    if (buffer < 0) return nullptr;
    const std::vector<int>& pieces = g_state.pieces[buffer];
    if (index >= pieces.size() || pieces[index] < 0) return nullptr;
    return g_state.colors[buffer][pieces[index]].data();
}

}  // namespace

void hostPalettesRegister()
{
    gba_mod_register_function_entry_plugin(kComposeHookId, kComposeOam, 1, composeEntry);
    gba_mod_register_function_entry_plugin(kParticleHookId, kDrawParticle, 1, particleEntry);
}

void hostPalettesApply(bool twoTowersData)
{
    g_state = State{};
    g_state.enabled = twoTowersData;
    gba_mod_set_function_hook_enabled(kComposeHookId, g_state.enabled);
    gba_mod_set_function_hook_enabled(kParticleHookId, g_state.enabled);
    gba::g_obj_palette_provider = g_state.enabled ? paletteProvider : nullptr;
}

bool hostPalettesActive()
{
    return g_state.enabled;
}

void hostPalettesNotePiece(int buffer, std::uint32_t index)
{
    if (!g_state.enabled || buffer < 0 || buffer > 1) return;
    std::vector<int>& pieces = g_state.pieces[buffer];
    if (index == 0) {
        // a new frame in this buffer
        pieces.clear();
        g_state.colors[buffer].clear();
        g_state.addresses[buffer].clear();
    }
    if (pieces.size() <= index) pieces.resize(index + 1u, -1);
    const std::uint32_t palette = ownPalette(g_state.actor);
    pieces[index] = palette ? colorsIndex(buffer, palette) : -1;
    ++g_state.pushed;
    if (palette) ++g_state.owned;
}

void hostPalettesSetDisplayed(int buffer)
{
    g_state.displayed = buffer;
}

}  // namespace rotk
