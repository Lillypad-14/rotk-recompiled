#include "widescreen.h"

#include <cstdio>
#include <cstdlib>

#include "gba_ppu.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

// How ROTK draws a level (see docs/WIDESCREEN.md):
//
// - BG3 and BG2 are the world layers. Each frame the game copies the visible 31x21 tiles into their
//   screenblocks; the hardware scroll only adds the 0..7 pixel fine offset. VRAM column 0 is world
//   tile camera.x >> 3, row 0 is world tile camera.y >> 3.
// - The whole level stays in EWRAM as metatiles: per layer a u16 cell map (one cell per 4x4 tiles)
//   and a block table of 16 tilemap entries per cell value.
// - BG1 is a repeating 256x256 parallax pattern that wraps on its own.
//
// The margins left and right of the native 240 columns are drawn from the cell maps, and the camera
// is clamped so the whole wide view stays inside the level.

extern "C" unsigned g_ws_active;
extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

namespace rotk {
namespace {

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "widescreen";
constexpr std::uint16_t kMaxViewWidth = 400;

constexpr std::uint32_t kSceneCurrentId = 0x030041B0u;   // g_SceneCurrent.id
constexpr std::uint32_t kSceneGameplay = 1u;
constexpr std::uint32_t kCameraPositions = 0x030020F8u;  // Vector2Int g_CameraPositions[2]
constexpr std::uint32_t kActivePlayerIndex = 0x03003F59u;  // g_PlayerGlobals.activePlayerIndex
constexpr std::uint32_t kWorldWidthTiles = 0x03005F90u;
constexpr std::uint32_t kWorldHeightTiles = 0x03005F88u;
constexpr std::uint32_t kCellMapWidth = 0x03005878u;     // u16, cells per row (4 tiles each)
constexpr std::uint32_t kBg3CellMap = 0x03005FB4u;       // u16 *, after the 4-byte header
constexpr std::uint32_t kBg3Blocks = 0x03005FBCu;
constexpr std::uint32_t kBg2CellMap = 0x0300586Cu;
constexpr std::uint32_t kBg2Blocks = 0x03005FC0u;
constexpr std::uint32_t kVramBase = 0x06000000u;
constexpr std::uint32_t kCameraCopies = 0x03005880u;     // Vector2Int[2], copy of the clamped camera
constexpr std::uint32_t kFineScrollX = 0x03005FB8u;      // u16, camera.x & 7 of the active player
constexpr std::uint32_t kCallViaSl = 0x0804E0B8u;        // _call_via_sl
constexpr std::uint32_t kCameraCommitReturn = 0x0803EC49u;  // its call in gfx_updatePlayerCameraScroll
constexpr const char* kCameraHookId = "rotk.pc.widescreen-camera";
constexpr std::uint32_t kUpdateVisibility = 0x08000C38u;  // actor_updateVisibility(Actor *)
constexpr const char* kVisibilityHookId = "rotk.pc.widescreen-visibility";
constexpr std::uint32_t kActivePlayerCount = 0x03003F58u;  // g_PlayerGlobals.activePlayerCount
constexpr std::uint32_t kActorCoords = 0xACu;
constexpr std::uint32_t kActorFlags = 0xE8u;
constexpr std::uint32_t kActorField0xFE = 0xFEu;
constexpr std::uint32_t kActorListId = 0xFFu;
constexpr std::uint32_t kActorField0x100 = 0x100u;
constexpr std::uint32_t kFlagRender = 1u << 0;
constexpr std::uint32_t kFlagOnScreen = 1u << 12;
constexpr std::uint32_t kFlagAlwaysVisible = 1u << 21;
constexpr std::uint32_t kFlagOnActivePlayerScreen = 1u << 30;
constexpr std::uint32_t kListOverlay = 1u;

struct WorldLayer {
    std::uint32_t cellMap = 0;
    std::uint32_t blocks = 0;
    int hofs = 0;
    int vofs = 0;
};

struct FrameState {
    bool world = false;          // gameplay with the world layers in use
    int cameraX = 0;             // latched with the frame's world-row copy
    int cameraY = 0;
    int worldTilesX = 0;
    int worldTilesY = 0;
    std::uint32_t cellsPerRow = 0;
    std::uint32_t bg3Screenblock = 0;
    WorldLayer layers[4];        // indexed by BG; only 2 and 3 are used
};

FrameState g_frame;
int g_latchedCameraX = 0;
int g_latchedCameraY = 0;

std::uint16_t ioHalf(const std::uint8_t* io, std::size_t offset)
{
    return static_cast<std::uint16_t>(io[offset] | (io[offset + 1] << 8));
}

int floorDiv8(int value)
{
    return value >= 0 ? value >> 3 : -((7 - value) >> 3);
}

void latchCamera()
{
    const std::uint32_t player = bus_read_u8(kActivePlayerIndex) & 1u;
    g_latchedCameraX = static_cast<int>(bus_read_u32(kCameraPositions + player * 8u));
    g_latchedCameraY = static_cast<int>(bus_read_u32(kCameraPositions + player * 8u + 4u));
}

extern "C" int rotkWorldTile(int bg, int hardwareX, int screenY, std::uint16_t* entry)
{
    if (bg == 1) return gba::kWsTilemapKeepWrapped;
    if (!g_frame.world || (bg != 2 && bg != 3)) return gba::kWsTilemapUnavailable;
    const WorldLayer& layer = g_frame.layers[bg];
    const int tileX = (g_frame.cameraX >> 3) + floorDiv8(hardwareX + layer.hofs);
    const int tileY = (g_frame.cameraY >> 3) + floorDiv8(screenY + layer.vofs);
    if (tileX < 0 || tileY < 0 || tileX >= g_frame.worldTilesX || tileY >= g_frame.worldTilesY)
        return gba::kWsTilemapUnavailable;
    const std::uint32_t cell = static_cast<std::uint32_t>((tileY >> 2) * static_cast<int>(g_frame.cellsPerRow) + (tileX >> 2));
    const std::uint16_t block = bus_read_u16(layer.cellMap + cell * 2u);
    *entry = bus_read_u16(layer.blocks + block * 32u + static_cast<std::uint32_t>((tileY & 3) * 8 + (tileX & 3) * 2));
    return gba::kWsTilemapReplace;
}

// gfx_updatePlayerCameraScroll clamps the active player's camera to [0, worldWidth - 240], stores it
// and then calls the scroll commit through _call_via_sl. At that call the camera is re-clamped to
// [left margin, worldWidth - 240 - right margin], which equals clamping the unclamped camera to the
// wide view. Levels narrower than the wide view are centred.
int clampCameraToView(std::uint32_t, int, ArmCpuState* cpu)
{
    if (cpu->R[14] != kCameraCommitReturn || !g_ws_active ||
        bus_read_u32(kSceneCurrentId) != kSceneGameplay)
        return 0;
    const std::uint32_t player = cpu->R[0] & 1u;
    const int worldWidth = bus_read_u16(kWorldWidthTiles) * 8;
    const int low = static_cast<int>(g_ws_extra_left);
    const int high = worldWidth - 240 - static_cast<int>(g_ws_extra_right);
    const int camera = static_cast<int>(bus_read_u32(kCameraPositions + player * 8u));
    int clamped = camera < low ? low : camera > high ? high : camera;
    if (high < low) clamped = (low + high) / 2;
    if (clamped == camera) return 0;
    bus_write_u32(kCameraPositions + player * 8u, static_cast<std::uint32_t>(clamped));
    bus_write_u32(kCameraCopies + player * 8u, static_cast<std::uint32_t>(clamped));
    bus_write_u16(kFineScrollX, static_cast<std::uint16_t>(clamped & 7));
    return 0;
}

void returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    else cpu->cpsr &= ~0x20u;
}

// actor_updateVisibility (0x08000C38, src/c/actor_updateVisibility.c in the decompilation) with the
// camera box widened by the margins. Actors without the on-screen flag neither tick nor draw, so
// this keeps everything inside the wide view alive.
int updateVisibilityWide(std::uint32_t, int, ArmCpuState* cpu)
{
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[rotk] host actor visibility active, margins %u/%u\n", g_ws_extra_left, g_ws_extra_right);
    }
    const std::uint32_t actor = cpu->R[0];
    std::uint32_t flags = bus_read_u32(actor + kActorFlags);
    if (bus_read_u8(actor + kActorField0xFE) == 1u) flags |= kFlagOnScreen | kFlagOnActivePlayerScreen;
    if ((flags & kFlagAlwaysVisible) || bus_read_u8(actor + kActorListId) == kListOverlay) {
        bus_write_u32(actor + kActorFlags, flags | kFlagOnScreen | kFlagOnActivePlayerScreen);
        returnToCaller(cpu);
        return 1;
    }
    const std::uint32_t activeCount = bus_read_u8(kActivePlayerCount);
    const std::uint32_t cameraCount = activeCount != 0 ? activeCount : 1u;
    const std::uint32_t activePlayer = bus_read_u8(kActivePlayerIndex);
    const int x = static_cast<std::int16_t>(bus_read_u32(actor + kActorCoords) >> 16);
    const int y = static_cast<std::int16_t>(bus_read_u32(actor + kActorCoords + 4u) >> 16);
    const int margin = (flags & kFlagRender) && (bus_read_u8(actor + kActorField0x100) & 0x10u) ? 100 : 40;
    for (std::uint32_t player = 0; player < cameraCount; ++player) {
        const int cameraX = static_cast<int>(bus_read_u32(kCameraPositions + player * 8u));
        const int cameraY = static_cast<int>(bus_read_u32(kCameraPositions + player * 8u + 4u));
        if (x >= cameraX - margin - static_cast<int>(g_ws_extra_left) && y >= cameraY - margin &&
            x <= cameraX + 240 + margin + static_cast<int>(g_ws_extra_right) && y <= cameraY + 160 + margin) {
            flags |= kFlagOnScreen;
            if (player == activePlayer) flags |= kFlagOnActivePlayerScreen;
        }
    }
    bus_write_u32(actor + kActorFlags, flags);
    returnToCaller(cpu);
    return 1;
}

void widescreenFrame(const gbarecomp::ExtendedViewFrameInfo* info)
{
    const std::uint8_t* io = info->io;
    const std::uint16_t dispcnt = io ? ioHalf(io, 0) : 0;
    g_frame.world = io && bus_read_u32(kSceneCurrentId) == kSceneGameplay && (dispcnt & 7u) == 0 &&
                    (dispcnt & 0x0C00u) == 0x0C00u;
    gba::g_ws_pillarbox = g_frame.world ? 0 : 1;
    gba::g_ws_tilemap_provider = g_frame.world ? rotkWorldTile : nullptr;
    if (!g_frame.world) return;
    g_frame.cameraX = g_latchedCameraX;
    g_frame.cameraY = g_latchedCameraY;
    g_frame.worldTilesX = bus_read_u16(kWorldWidthTiles);
    g_frame.worldTilesY = bus_read_u16(kWorldHeightTiles);
    g_frame.cellsPerRow = bus_read_u16(kCellMapWidth);
    g_frame.bg3Screenblock = kVramBase + ((ioHalf(io, 0x0E) >> 8) & 31u) * 0x800u;
    g_frame.layers[3].cellMap = bus_read_u32(kBg3CellMap);
    g_frame.layers[3].blocks = bus_read_u32(kBg3Blocks);
    g_frame.layers[2].cellMap = bus_read_u32(kBg2CellMap);
    g_frame.layers[2].blocks = bus_read_u32(kBg2Blocks);
    for (int bg = 2; bg <= 3; ++bg) {
        g_frame.layers[bg].hofs = ioHalf(io, 0x10 + bg * 4) & 0x1FF;
        g_frame.layers[bg].vofs = ioHalf(io, 0x12 + bg * 4) & 0x1FF;
    }
}

void widescreenInit(std::uint32_t, std::uint32_t)
{
    gba::g_ws_authored_margin_layers = 1;
    latchCamera();
}

// ROTK_PC_TEST_VISIBILITY (test only): run the host actor_updateVisibility without widescreen. With no
// margins it must reproduce the original frames exactly.
void widescreenReset()
{
    if (std::getenv("ROTK_PC_TEST_VISIBILITY")) gba_mod_set_function_hook_enabled(kVisibilityHookId, 1);
}

void widescreenActivated()
{
    const char* width = gba_mod_option_value(kPackageId, kFeatureId, "width");
    gba_mod_set_view_width(width ? std::atoi(width) : 284);
    gba_mod_set_function_hook_enabled(kCameraHookId, 1);
    gba_mod_set_function_hook_enabled(kVisibilityHookId, 1);
}

}  // namespace

void widescreenRegister(gbarecomp::RunOptions& options)
{
    gba_mod_register_activation_plugin("rotk.pc.widescreen", widescreenActivated);
    gba_mod_register_reset_callback(widescreenReset);
    gba_mod_register_function_entry_plugin(kCameraHookId, kCallViaSl, 1, clampCameraToView);
    gba_mod_register_function_entry_plugin(kVisibilityHookId, kUpdateVisibility, 1, updateVisibilityWide);
    options.mod_owns_adaptive_view = true;
    options.max_view_width = kMaxViewWidth;
    options.extended_view_init = widescreenInit;
    options.extended_view_frame = widescreenFrame;
}

void widescreenObserveDma(std::uint32_t, std::uint32_t destination)
{
    if (g_frame.bg3Screenblock != 0 && destination >= g_frame.bg3Screenblock &&
        destination < g_frame.bg3Screenblock + 0x800u)
        latchCamera();
}

}  // namespace rotk
