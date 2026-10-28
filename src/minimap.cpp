#include "minimap.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "mod_runtime.h"

namespace rotk {
namespace {

// A minimap in the top right of the gameplay screen, drawn into each shown frame: the level's
// collision map, the view, both players and the enemies. Read from the game's memory, presentation
// only.
//
// terrain_getCollisionAt (0x08034770): the world is a grid of 32x32 px blocks. *kBlockMapLiteral
// points to one u16 per block, row-major (block id in the low 12 bits), and *kCollisionLiteral to
// 16 collision bytes per block id, one per 8x8 tile, 0 where actors walk.

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "minimap";
constexpr std::uint32_t kBlockMapLiteral = 0x080347D0u;   // address of the block map pointer
constexpr std::uint32_t kCollisionLiteral = 0x080347D4u;  // address of the collision table pointer
constexpr std::uint32_t kWorldWidthTiles = 0x03005F90u;   // u16 g_WorldWidthTiles
constexpr std::uint32_t kWorldHeightTiles = 0x03005F88u;  // u16 g_WorldHeightTiles
constexpr std::uint32_t kCameraPositions = 0x030020F8u;   // Vector2Int g_CameraPositions[2]
constexpr std::uint32_t kActivePlayer = 0x03003F59u;      // g_PlayerGlobals.activePlayerIndex
constexpr std::uint32_t kPlayers = 0x03003C00u;           // g_PlayerGlobals.players[2], actor first
constexpr std::uint32_t kPlayerSize = 0x1ACu;
constexpr std::uint32_t kSceneCurrentId = 0x030041B0u;
constexpr std::uint32_t kGameFlags = 0x03004144u;
constexpr std::uint32_t kSceneGameplay = 1u;
constexpr std::uint32_t kWorldHidden = (1u << 3) | (1u << 22);  // GAME_FLAG_WORLD_PARKED, _OVERLAY
constexpr std::uint32_t kActorListMain = 0x03002420u;
constexpr std::uint32_t kActorOwnerChar = 0x08u;
constexpr std::uint32_t kActorHp = 0x5Au;
constexpr std::uint32_t kActorUnitType = 0x61u;
constexpr std::uint32_t kActorRenderX = 0xCCu;
constexpr std::uint32_t kActorRenderY = 0xD0u;
constexpr std::uint32_t kActorType = 0xF0u;
constexpr std::uint8_t kUnitContainer = 0x53u;
constexpr std::uint16_t kTypeLootPile = 10u, kTypeChest = 12u, kTypeGemRain = 16u;
constexpr int kBlock = 32;     // world pixels per block
constexpr int kMaxSize = 72;   // the minimap's largest side in native pixels
constexpr int kMargin = 4;

struct Map {
    std::uint32_t blocks = 0;  // the block map it was built from
    int worldW = 0, worldH = 0;
    int width = 0, height = 0, blocksPerPixel = 1;
    std::vector<std::uint8_t> open;  // per minimap pixel: walkable tiles out of 16 per block
};

bool g_enabled = false;
bool g_enemies = true;
Map g_maps[3];  // the normal game, co-op console 1, co-op console 2

void build(gba::GbaBus& bus, Map& map, std::uint32_t blocks, int worldW, int worldH)
{
    const std::uint32_t collision = bus.read32(bus.read32(kCollisionLiteral));
    const int blocksW = worldW / kBlock, blocksH = worldH / kBlock;
    map = Map{};
    map.blocks = blocks;
    map.worldW = worldW;
    map.worldH = worldH;
    map.blocksPerPixel = std::max(1, (std::max(blocksW, blocksH) + kMaxSize - 1) / kMaxSize);
    map.width = (blocksW + map.blocksPerPixel - 1) / map.blocksPerPixel;
    map.height = (blocksH + map.blocksPerPixel - 1) / map.blocksPerPixel;
    map.open.assign(std::size_t(map.width) * map.height, 0);
    for (int by = 0; by < blocksH; ++by)
        for (int bx = 0; bx < blocksW; ++bx) {
            const std::uint32_t id = bus.read16(blocks + (std::uint32_t(by) * blocksW + bx) * 2u) & 0xFFFu;
            int open = 0;
            for (int tile = 0; tile < 16; ++tile) open += bus.read8(collision + id * 16u + tile) == 0;
            auto& cell = map.open[std::size_t(by / map.blocksPerPixel) * map.width + bx / map.blocksPerPixel];
            cell = static_cast<std::uint8_t>(std::max<int>(cell, open));
        }
}

struct Color {
    int r, g, b;
};

void pixel(const gbarecomp::PresentFrame& f, int x, int y, Color c, int alpha)
{
    const int scale = static_cast<int>(f.scale);
    if (x < 0 || y < 0 || (x + 1) * scale > static_cast<int>(f.width) || (y + 1) * scale > static_cast<int>(f.height))
        return;
    for (int dy = 0; dy < scale; ++dy) {
        std::uint8_t* row = f.rgb + (std::size_t(y * scale + dy) * f.stride + x * scale) * 3u;
        for (int dx = 0; dx < scale; ++dx, row += 3) {
            row[0] = static_cast<std::uint8_t>(row[0] + (c.r - row[0]) * alpha / 256);
            row[1] = static_cast<std::uint8_t>(row[1] + (c.g - row[1]) * alpha / 256);
            row[2] = static_cast<std::uint8_t>(row[2] + (c.b - row[2]) * alpha / 256);
        }
    }
}

bool option(const char* env, const char* id, bool fallback)
{
    if (const char* value = std::getenv(env)) return std::string(value) != "0";
    const char* value = gba_mod_option_value(kPackageId, kFeatureId, id);
    return value ? std::string(value) == "true" : fallback;
}

void minimapActivated()
{
    g_enabled = true;
    g_enemies = option("ROTK_PC_MINIMAP_ENEMIES", "enemies", true);
}

void minimapReset()
{
    g_enabled = false;
    for (Map& map : g_maps) map = Map{};
    if (const char* env = std::getenv("ROTK_PC_MINIMAP"); env && std::string(env) != "0") minimapActivated();
}

}  // namespace

bool minimapActive()
{
    return g_enabled;
}

void minimapDecorate(const gbarecomp::PresentFrame* frame)
{
    if (!g_enabled || !frame->bus || frame->console < -1 || frame->console > 1) return;
    gba::GbaBus& bus = *frame->bus;
    if (bus.read32(kSceneCurrentId) != kSceneGameplay || (bus.read32(kGameFlags) & kWorldHidden)) return;
    const int worldW = bus.read16(kWorldWidthTiles) * 8, worldH = bus.read16(kWorldHeightTiles) * 8;
    const std::uint32_t blocks = bus.read32(bus.read32(kBlockMapLiteral));
    if (worldW < kBlock || worldH < kBlock || !blocks) return;
    Map& map = g_maps[frame->console + 1];
    if (map.blocks != blocks || map.worldW != worldW || map.worldH != worldH) build(bus, map, blocks, worldW, worldH);

    const int screenW = static_cast<int>(frame->width / frame->scale);
    const int left = screenW - kMargin - map.width, top = kMargin;
    const int pixelWorld = kBlock * map.blocksPerPixel;  // world pixels per minimap pixel
    const gbarecomp::PresentFrame& f = *frame;
    for (int y = -1; y <= map.height; ++y)
        for (int x = -1; x <= map.width; ++x) {
            const bool edge = x < 0 || y < 0 || x == map.width || y == map.height;
            const int open = edge ? -1 : map.open[std::size_t(y) * map.width + x];
            if (edge) pixel(f, left + x, top + y, {16, 16, 16}, 200);
            else if (open >= 8) pixel(f, left + x, top + y, {150, 150, 140}, 170);
            else if (open > 0) pixel(f, left + x, top + y, {90, 90, 84}, 170);
            else pixel(f, left + x, top + y, {20, 20, 28}, 150);
        }
    const auto dot = [&](int worldX, int worldY, Color c, int size) {
        const int x = left + worldX / pixelWorld, y = top + worldY / pixelWorld;
        for (int dy = 0; dy < size; ++dy)
            for (int dx = 0; dx < size; ++dx) pixel(f, x + dx - size / 2, y + dy - size / 2, c, 256);
    };
    // The view: this console's camera, a 240x160 rectangle.
    const std::uint32_t camera = kCameraPositions + (bus.read8(kActivePlayer) & 1u) * 8u;
    const int cameraX = static_cast<std::int32_t>(bus.read32(camera)) / pixelWorld;
    const int cameraY = static_cast<std::int32_t>(bus.read32(camera + 4u)) / pixelWorld;
    const int viewW = std::max(1, 240 / pixelWorld), viewH = std::max(1, 160 / pixelWorld);
    for (int i = 0; i <= viewW; ++i) {
        pixel(f, left + cameraX + i, top + cameraY, {240, 240, 240}, 140);
        pixel(f, left + cameraX + i, top + cameraY + viewH, {240, 240, 240}, 140);
    }
    for (int j = 0; j <= viewH; ++j) {
        pixel(f, left + cameraX, top + cameraY + j, {240, 240, 240}, 140);
        pixel(f, left + cameraX + viewW, top + cameraY + j, {240, 240, 240}, 140);
    }
    if (g_enemies) {
        std::uint32_t actor = bus.read32(kActorListMain);
        for (unsigned count = 0; actor && count < 4096u; ++count, actor = bus.read32(actor)) {
            const std::uint16_t type = bus.read16(actor + kActorType);
            if (type == kTypeLootPile || type == kTypeChest || type == kTypeGemRain ||
                bus.read32(actor + kActorOwnerChar) != 0 || bus.read8(actor + kActorUnitType) >= kUnitContainer ||
                static_cast<std::int16_t>(bus.read16(actor + kActorHp)) <= 0)
                continue;
            dot(static_cast<std::int32_t>(bus.read32(actor + kActorRenderX)) >> 16,
                static_cast<std::int32_t>(bus.read32(actor + kActorRenderY)) >> 16, {224, 56, 40}, 1);
        }
    }
    // Both players, this console's own on top.
    const unsigned own = bus.read8(kActivePlayer) & 1u;
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned p = i == 0 ? own ^ 1u : own;
        const std::uint32_t actor = bus.read32(kPlayers + p * kPlayerSize);
        if (!actor) continue;
        dot(static_cast<std::int32_t>(bus.read32(actor + kActorRenderX)) >> 16,
            static_cast<std::int32_t>(bus.read32(actor + kActorRenderY)) >> 16,
            p == own ? Color{80, 220, 255} : Color{255, 210, 70}, 2);
    }
}

void minimapRegister()
{
    gba_mod_register_activation_plugin("rotk.pc.minimap", minimapActivated);
    gba_mod_register_reset_callback(minimapReset);
}

}  // namespace rotk
