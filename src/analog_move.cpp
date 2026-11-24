#include "analog_move.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace rotk {
namespace {

// Stock cartridge layout (the PC build's decomp headers carry the Expanded
// mod's extra fields, so these are the host's pinned stock offsets).
constexpr std::uint32_t kPlayerTick = 0x080236B8u;            // actor_player_tick(Actor*)
constexpr std::uint32_t kSetVelocity = 0x08001C80u;           // actor_setVelocityFromDirection8
constexpr std::uint32_t kPlayerGlobals = 0x03003C00u;
constexpr std::uint32_t kPlayerStride = 0x1ACu;               // sizeof(Player)
constexpr std::uint32_t kActorMoveSpeed = 0xE4u;              // just before flags at 0xE8
constexpr std::uint32_t kDirections = 0x08282150u;            // Direction8MoveVectors
constexpr std::uint32_t kDirectionsSteep = 0x08282190u;

constexpr const char* kTickHookId = "rotk.pc-analog-player-tick";
constexpr const char* kVelocityHookId = "rotk.pc-analog-velocity";

bool g_enabled = true;
float g_forcedMagnitude = -1.0f;  // ROTK_PC_ANALOG_TEST, for tests without a pad
std::uint32_t g_tickActor = 0;
int g_tickPlayer = -1;
unsigned long long g_movementCalls = 0, g_scaledCalls = 0;
unsigned long long g_tickEntries = 0, g_velocityEntries = 0;

// Set by the framework's input pump in host_window.cpp.
extern "C" float gba_analog_move[2];

void returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    else cpu->cpsr &= ~0x20u;
}

// The game's 16.16 multiply (math_multiplyFp16).
std::int32_t multiplyFp16(std::int32_t a, std::int32_t b)
{
    return ((a >> 6) * (b >> 6)) >> 4;
}

int playerTickEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    g_tickActor = 0;
    g_tickPlayer = -1;
    if (!g_enabled) return 0;
    const std::uint32_t actor = cpu->R[0];
    ++g_tickEntries;
    if (std::getenv("ROTK_PC_ANALOG_DEBUG") && g_tickEntries <= 5)
        std::fprintf(stderr, "[analog] player tick actor=%08x\n", actor);
    for (int player = 0; player < 2; ++player) {
        if (bus_read_u32(kPlayerGlobals + static_cast<std::uint32_t>(player) * kPlayerStride) == actor) {
            g_tickActor = actor;
            g_tickPlayer = player;
            break;
        }
    }
    return 0;
}

// actor_setVelocityFromDirection8(Vector2Fp16 *out, u32 flags, s32 direction, s32 speed).
// Replace the call only for the player's own movement call; knockback, other
// actors and full-speed input run the original body.
int setVelocityEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t actor = g_tickActor;
    const int player = g_tickPlayer;
    g_tickActor = 0;
    g_tickPlayer = -1;
    ++g_velocityEntries;
    if (std::getenv("ROTK_PC_ANALOG_DEBUG") && g_velocityEntries <= 5)
        std::fprintf(stderr, "[analog] velocity out=%08x tick=%08x player=%d arg=%08x\n",
                     cpu->R[0], actor, player, cpu->R[3]);
    if (!g_enabled || !actor || player < 0) return 0;
    ++g_movementCalls;
    const float magnitude = g_forcedMagnitude >= 0.0f ? g_forcedMagnitude : gba_analog_move[player];
    if (!(magnitude > 0.0f) || magnitude >= 0.999f) return 0;
    // The player's movement call passes its own moveSpeed and may write through
    // a temporary vector that the tick then copies into the actor.
    if (cpu->R[3] != bus_read_u32(actor + kActorMoveSpeed)) return 0;
    const std::int32_t direction = static_cast<std::int32_t>(cpu->R[2]);
    if (direction < 0 || direction > 7) return 0;

    const std::uint32_t table = (cpu->R[1] & (1u << 24)) ? kDirectionsSteep : kDirections;
    const std::int32_t scaled = static_cast<std::int32_t>(std::lround(
        static_cast<double>(static_cast<std::int32_t>(cpu->R[3])) * magnitude));
    const std::int32_t x = static_cast<std::int32_t>(bus_read_u32(table + static_cast<std::uint32_t>(direction) * 8u));
    const std::int32_t y = static_cast<std::int32_t>(bus_read_u32(table + static_cast<std::uint32_t>(direction) * 8u + 4u));
    bus_write_u32(cpu->R[0], static_cast<std::uint32_t>(multiplyFp16(x, scaled)));
    bus_write_u32(cpu->R[0] + 4u, static_cast<std::uint32_t>(multiplyFp16(y, scaled)));
    ++g_scaledCalls;
    if (std::getenv("ROTK_PC_ANALOG_DEBUG"))
        std::fprintf(stderr, "[analog] player %d direction %d speed %u -> %d\n",
                     player, direction, cpu->R[3], scaled);
    returnToCaller(cpu);
    return 1;
}

void analogActivated()
{
    if (!g_enabled) return;
    gba_mod_set_function_hook_enabled(kTickHookId, 1);
    gba_mod_set_function_hook_enabled(kVelocityHookId, 1);
}

}  // namespace

void analogMoveRegister()
{
    if (const char* env = std::getenv("ROTK_PC_ANALOG_MOVE"))
        if (env[0] == '0') g_enabled = false;
    // Test-only: force a stick magnitude without a controller.
    if (const char* env = std::getenv("ROTK_PC_ANALOG_TEST")) {
        const float value = std::strtof(env, nullptr);
        if (value >= 0.0f && value <= 1.0f) g_forcedMagnitude = value;
    }
    std::atexit([] {
        if (g_tickEntries)
            std::fprintf(stderr, "[rotk] analog movement: %llu ticks, %llu velocity calls, %llu player calls, %llu scaled\n",
                         g_tickEntries, g_velocityEntries, g_movementCalls, g_scaledCalls);
    });
    const int tick_registered = gba_mod_register_function_entry_plugin(kTickHookId, kPlayerTick, 1, playerTickEntry);
    const int velocity_registered = gba_mod_register_function_entry_plugin(kVelocityHookId, kSetVelocity, 1, setVelocityEntry);
    if (g_enabled) {
        gba_mod_set_function_hook_enabled(kTickHookId, 1);
        gba_mod_set_function_hook_enabled(kVelocityHookId, 1);
    }
    (void)tick_registered;
    (void)velocity_registered;
    gba_mod_register_activation_plugin("rotk.pc.analog-move", analogActivated);
}

}  // namespace rotk
