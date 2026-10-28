#include "combat_info.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "gba_bus.h"
#include "mod_runtime.h"

namespace rotk {
namespace {

// Enemy health bars and damage numbers, drawn into each shown frame (combatInfoDecorate).
// Presentation only: the game's memory is read, never written, so the simulation and co-op are
// unchanged. Bars and numbers use native-pixel geometry scaled to the internal resolution, so they
// look like the GBA's own graphics.

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "combat-info";
constexpr std::uint32_t kActorListMain = 0x03002420u;    // ActorList g_ActorListMain (head first)
constexpr std::uint32_t kCameraPositions = 0x030020F8u;  // Vector2Int g_CameraPositions[2]
constexpr std::uint32_t kActivePlayer = 0x03003F59u;     // g_PlayerGlobals.activePlayerIndex
constexpr std::uint32_t kSceneCurrentId = 0x030041B0u;   // g_SceneCurrent.id
constexpr std::uint32_t kGameFlags = 0x03004144u;        // g_GameFlags
constexpr std::uint32_t kSceneGameplay = 1u;
// GAME_FLAG_WORLD_PARKED and GAME_FLAG_OVERLAY: a menu is over the world.
constexpr std::uint32_t kWorldHidden = (1u << 3) | (1u << 22);

// Actor fields (game/include/actor.h).
constexpr std::uint32_t kActorNext = 0x00u;
constexpr std::uint32_t kActorOwnerChar = 0x08u;  // Player *, set for player avatars
constexpr std::uint32_t kActorHp = 0x5Au;         // s16 currentHp
constexpr std::uint32_t kActorUnitType = 0x61u;   // indexes EnemyTypeInfos below kUnitContainer
constexpr std::uint32_t kActorRenderX = 0xCCu;    // fp16 renderCoords
constexpr std::uint32_t kActorRenderY = 0xD0u;
constexpr std::uint32_t kActorType = 0xF0u;       // u16 ActorType
constexpr std::uint8_t kUnitContainer = 0x53u;    // LOOT_UNIT_TYPE_CONTAINER: chests and up
// Actor types that reuse currentHp for something else (ACTOR_TYPE_LOOT_PILE, _CHEST, _GEM_RAIN).
// Every other non-player actor with an enemy unit type is an enemy: mission-placed ones (type 25) and
// spawned ones alike.
constexpr std::uint16_t kTypeLootPile = 10u, kTypeChest = 12u, kTypeGemRain = 16u;
constexpr unsigned kMaxActors = 4096u;

// Bar: kBarWidth x kBarHeight inside a 1-pixel outline, kBarRise pixels above the actor's position.
constexpr int kBarWidth = 16, kBarHeight = 2, kBarRise = 30;
// A damage number rises kPopupRise pixels over kPopupFrames presented frames and fades out over the
// last kPopupFade.
constexpr int kPopupFrames = 48, kPopupFade = 16, kPopupRise = 14;

struct Track {
    int maxHp = 0;
    int lastHp = 0;
    std::uint16_t type = 0;
    std::uint64_t seen = 0;
};

struct Popup {
    int worldX = 0, worldY = 0;  // native pixels
    int value = 0;
    int age = 0;
};

struct State {
    std::unordered_map<std::uint32_t, Track> tracks;
    std::vector<Popup> popups;
    std::uint64_t frame = 0;
};

bool g_enabled = false;
bool g_bars = true, g_numbers = true, g_alwaysBars = false;
State g_states[3];  // the normal game, co-op console 1, co-op console 2

// 3x5 digits, one row per byte, bit 2 leftmost.
constexpr std::uint8_t kDigits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 3, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

struct Color {
    int r, g, b;
};

struct Canvas {
    const gbarecomp::PresentFrame& frame;

    // Blends a native pixel at alpha/256.
    void pixel(int x, int y, Color c, int alpha = 256) const
    {
        const int scale = static_cast<int>(frame.scale);
        if (x < 0 || y < 0 || (x + 1) * scale > static_cast<int>(frame.width) ||
            (y + 1) * scale > static_cast<int>(frame.height))
            return;
        for (int dy = 0; dy < scale; ++dy) {
            std::uint8_t* row = frame.rgb + (std::size_t(y * scale + dy) * frame.stride + x * scale) * 3u;
            for (int dx = 0; dx < scale; ++dx, row += 3) {
                row[0] = static_cast<std::uint8_t>(row[0] + (c.r - row[0]) * alpha / 256);
                row[1] = static_cast<std::uint8_t>(row[1] + (c.g - row[1]) * alpha / 256);
                row[2] = static_cast<std::uint8_t>(row[2] + (c.b - row[2]) * alpha / 256);
            }
        }
    }

    void rect(int x, int y, int w, int h, Color c, int alpha = 256) const
    {
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) pixel(x + i, y + j, c, alpha);
    }

    // A number centred on x with its top at y, outlined so it reads on any background.
    void number(int value, int x, int y, Color c, int alpha) const
    {
        const std::string text = std::to_string(value);
        const int width = static_cast<int>(text.size()) * 4 - 1;
        const int left = x - width / 2;
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t i = 0; i < text.size(); ++i) {
                const auto& glyph = kDigits[text[i] - '0'];
                for (int row = 0; row < 5; ++row)
                    for (int col = 0; col < 3; ++col) {
                        if (!(glyph[row] & (4 >> col))) continue;
                        const int px = left + static_cast<int>(i) * 4 + col, py = y + row;
                        if (pass == 0) {
                            for (int oy = -1; oy <= 1; ++oy)
                                for (int ox = -1; ox <= 1; ++ox) pixel(px + ox, py + oy, {16, 16, 16}, alpha);
                        } else {
                            pixel(px, py, c, alpha);
                        }
                    }
            }
    }
};

Color barColor(int hp, int maxHp)
{
    if (hp * 2 > maxHp) return {72, 208, 72};
    if (hp * 4 > maxHp) return {232, 200, 48};
    return {224, 56, 40};
}

}  // namespace

void combatInfoDecorate(const gbarecomp::PresentFrame* frame)
{
    if (!g_enabled || !frame->bus || frame->console < -1 || frame->console > 1) return;
    gba::GbaBus& bus = *frame->bus;
    State& state = g_states[frame->console + 1];
    ++state.frame;
    if (bus.read32(kSceneCurrentId) != kSceneGameplay || (bus.read32(kGameFlags) & kWorldHidden)) {
        state.tracks.clear();
        state.popups.clear();
        return;
    }
    const std::uint32_t camera = kCameraPositions + (bus.read8(kActivePlayer) & 1u) * 8u;
    const int cameraX = static_cast<std::int32_t>(bus.read32(camera));
    const int cameraY = static_cast<std::int32_t>(bus.read32(camera + 4u));
    const int left = static_cast<int>(frame->extra_left), top = static_cast<int>(frame->extra_top);
    const Canvas canvas{*frame};

    std::uint32_t actor = bus.read32(kActorListMain);
    for (unsigned count = 0; actor && count < kMaxActors; ++count, actor = bus.read32(actor + kActorNext)) {
        const std::uint16_t type = bus.read16(actor + kActorType);
        if (type == kTypeLootPile || type == kTypeChest || type == kTypeGemRain ||
            bus.read32(actor + kActorOwnerChar) != 0 || bus.read8(actor + kActorUnitType) >= kUnitContainer)
            continue;
        const int hp = static_cast<std::int16_t>(bus.read16(actor + kActorHp));
        const int worldX = static_cast<std::int32_t>(bus.read32(actor + kActorRenderX)) >> 16;
        const int worldY = static_cast<std::int32_t>(bus.read32(actor + kActorRenderY)) >> 16;
        auto found = state.tracks.find(actor);
        if (found == state.tracks.end() || found->second.type != type) {
            if (hp <= 0) continue;
            found = state.tracks.insert_or_assign(actor, Track{hp, hp, type, 0}).first;
        }
        Track& track = found->second;
        if (hp < track.lastHp && g_numbers)
            state.popups.push_back({worldX, worldY - kBarRise - 8, track.lastHp - (hp > 0 ? hp : 0), 0});
        track.lastHp = hp;
        if (hp > track.maxHp) track.maxHp = hp;
        track.seen = state.frame;
        if (!g_bars || hp <= 0 || (hp >= track.maxHp && !g_alwaysBars)) continue;
        const int x = worldX - cameraX + left - kBarWidth / 2, y = worldY - cameraY + top - kBarRise;
        canvas.rect(x - 1, y - 1, kBarWidth + 2, kBarHeight + 2, {16, 16, 16});
        canvas.rect(x, y, kBarWidth, kBarHeight, {72, 40, 40});
        canvas.rect(x, y, (kBarWidth * hp + track.maxHp - 1) / track.maxHp, kBarHeight, barColor(hp, track.maxHp));
    }
    for (auto it = state.tracks.begin(); it != state.tracks.end();)
        it = it->second.seen == state.frame ? std::next(it) : state.tracks.erase(it);

    std::size_t kept = 0;
    for (Popup& popup : state.popups) {
        if (++popup.age > kPopupFrames) continue;
        const int fade = kPopupFrames - popup.age;
        const int alpha = fade < kPopupFade ? 256 * fade / kPopupFade : 256;
        canvas.number(popup.value, popup.worldX - cameraX + left,
                      popup.worldY - cameraY + top - kPopupRise * popup.age / kPopupFrames, {255, 240, 160},
                      alpha);
        state.popups[kept++] = popup;
    }
    state.popups.resize(kept);
}

bool combatInfoActive()
{
    return g_enabled;
}

namespace {

bool option(const char* env, const char* id, bool fallback)
{
    if (const char* value = std::getenv(env)) return std::string(value) != "0";
    const char* value = gba_mod_option_value(kPackageId, kFeatureId, id);
    return value ? std::string(value) == "true" : fallback;
}

void combatInfoActivated()
{
    g_enabled = true;
    g_bars = option("ROTK_PC_HEALTH_BARS", "health-bars", true);
    g_numbers = option("ROTK_PC_DAMAGE_NUMBERS", "damage-numbers", true);
    g_alwaysBars = option("ROTK_PC_HEALTH_BARS_ALWAYS", "always", false);
}

void combatInfoReset()
{
    // ROTK_PC_COMBAT_INFO overrides the package feature (whose activation runs after this).
    const char* env = std::getenv("ROTK_PC_COMBAT_INFO");
    g_enabled = false;
    for (State& state : g_states) state = State{};
    if (env && std::string(env) != "0") combatInfoActivated();
}

}  // namespace

void combatInfoRegister()
{
    gba_mod_register_activation_plugin("rotk.pc.combat-info", combatInfoActivated);
    gba_mod_register_reset_callback(combatInfoReset);
}

}  // namespace rotk
