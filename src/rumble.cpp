#include "rumble.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gba_bus.h"
#include "mod_runtime.h"

namespace rotk {
namespace {

// Controller rumble when the console's own player is hurt or levels up. Read from the game's memory
// at each shown frame, so the game is unchanged. In co-op console 1 rumbles player 1's controller and
// console 2 player 2's.

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "rumble";
constexpr std::uint32_t kPlayers = 0x03003C00u;       // g_PlayerGlobals.players[2]
constexpr std::uint32_t kPlayerSize = 0x1ACu;
constexpr std::uint32_t kPlayerHp = 0x17Cu;           // s16 Player.currentHp
constexpr std::uint32_t kPlayerLevel = 0x19Cu;        // u8 Player.level
constexpr std::uint32_t kActivePlayer = 0x03003F59u;  // g_PlayerGlobals.activePlayerIndex
constexpr std::uint32_t kSceneCurrentId = 0x030041B0u;
constexpr std::uint32_t kSceneGameplay = 1u;

struct State {
    bool valid = false;
    int hp = 0;
    int level = 0;
};

bool g_enabled = false;
bool g_hits = true, g_levelUp = true;
int g_strength = 2;  // option "strength": 1 light, 2 normal, 3 strong
State g_states[3];   // the normal game, co-op console 1, co-op console 2

// ROTK_PC_RUMBLE_LOG=1 logs each rumble, for tests without a controller.
void rumble(int controller, std::uint16_t low, std::uint16_t high, std::uint32_t ms, const char* why)
{
    static const bool log = std::getenv("ROTK_PC_RUMBLE_LOG") != nullptr;
    if (log) std::fprintf(stderr, "[rotk] rumble controller %d %s: %04x/%04x %u ms\n", controller, why, low, high, ms);
    gbarecomp::runtime_rumble(controller, low, high, ms);
}

std::uint16_t motor(int base)
{
    return static_cast<std::uint16_t>(std::clamp(base * g_strength / 2, 0, 0xFFFF));
}

bool option(const char* env, const char* id, bool fallback)
{
    if (const char* value = std::getenv(env)) return std::string(value) != "0";
    const char* value = gba_mod_option_value(kPackageId, kFeatureId, id);
    return value ? std::string(value) == "true" : fallback;
}

void rumbleActivated()
{
    g_enabled = true;
    g_hits = option("ROTK_PC_RUMBLE_HITS", "hits", true);
    g_levelUp = option("ROTK_PC_RUMBLE_LEVEL_UP", "level-up", true);
    const char* strength = gba_mod_option_value(kPackageId, kFeatureId, "strength");
    g_strength = strength ? std::clamp(std::atoi(strength), 1, 3) : 2;
}

void rumbleReset()
{
    g_enabled = false;
    for (State& state : g_states) state = State{};
    if (const char* env = std::getenv("ROTK_PC_RUMBLE"); env && std::string(env) != "0") rumbleActivated();
}

}  // namespace

bool rumbleActive()
{
    return g_enabled;
}

void rumbleObserve(const gbarecomp::PresentFrame* frame)
{
    if (!g_enabled || !frame->bus || frame->console < -1 || frame->console > 1) return;
    gba::GbaBus& bus = *frame->bus;
    State& state = g_states[frame->console + 1];
    if (bus.read32(kSceneCurrentId) != kSceneGameplay) {
        state.valid = false;
        return;
    }
    const std::uint32_t player = kPlayers + (bus.read8(kActivePlayer) & 1u) * kPlayerSize;
    const int hp = static_cast<std::int16_t>(bus.read16(player + kPlayerHp));
    const int level = bus.read8(player + kPlayerLevel);
    const int controller = frame->console < 0 ? 0 : frame->console;
    if (state.valid) {
        // The level reads 0 until the player's slot is in place.
        if (g_levelUp && state.level > 0 && level > state.level)
            rumble(controller, motor(0x6000), motor(0xC000), 400, "level-up");
        else if (g_hits && hp < state.hp && hp >= 0) {
            const int damage = state.hp - hp;
            rumble(controller, motor(0x4000 + damage * 0x600), motor(0x2000),
                   static_cast<std::uint32_t>(std::min(90 + damage * 6, 300)), "hit");
        }
    }
    state = {true, hp, level};
}

void rumbleRegister()
{
    gba_mod_register_activation_plugin("rotk.pc.rumble", rumbleActivated);
    gba_mod_register_reset_callback(rumbleReset);
}

}  // namespace rotk
