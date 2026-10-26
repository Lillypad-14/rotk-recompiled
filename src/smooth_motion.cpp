#include "smooth_motion.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gba_ppu.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "transparency.h"

namespace rotk {
namespace {

// The game composes a tick into one OAM shadow buffer while the other is on screen, and the
// VBlank handler swaps them every other vblank (docs/SMOOTH_MOTION.md). On the repeated vblank the
// next tick is usually composed already (more so with the CPU boost), so the frame in between can
// show the halfway point:
//
//   camera    the world layers BG3/BG2 hold the visible tiles of the camera and scroll only by the
//             fine offset, so the presented camera moves half way within the copied tiles.
//   sprites   each piece is tagged with the actor whose sprite_composeOam pushed it. A displayed
//             piece moves by half of its actor's position change, minus the camera shift for world
//             actors.
//
//   particles fx_drawParticle pieces are tagged with their particle record the same way.
//   parallax  BG1 moves by its learnt ratio to the camera.
//
// Changes over kMaxStep pixels are scene cuts or teleports and stay as they are. The HUD does not
// move.

constexpr std::uint32_t kComposeOam = 0x08002484u;     // sprite_composeOam(Actor*)
constexpr std::uint32_t kDrawParticle = 0x08039324u;  // fx_drawParticle, the other piece pusher
constexpr std::uint32_t kCameraPositions = 0x030020F8u;  // Vector2Int g_CameraPositions[2]
constexpr std::uint32_t kActivePlayer = 0x03003F59u;     // g_PlayerGlobals.activePlayerIndex
constexpr std::uint32_t kSceneCurrentId = 0x030041B0u;  // g_SceneCurrent.id
constexpr std::uint32_t kSceneGameplay = 1u;
constexpr std::uint32_t kActorRenderX = 0xCCu;           // fp16 renderCoords
constexpr std::uint32_t kActorRenderY = 0xD0u;
constexpr std::uint32_t kActorFlags = 0xE8u;
constexpr std::uint32_t kActorListId = 0xFFu;
constexpr std::uint32_t kActorScreenSpace = 0x200000u;   // drawn without the camera
constexpr std::uint8_t kListOverlay = 1u;
constexpr std::uint32_t kParticleRenderX = 0x1Au;        // integer half of FxParticle.renderPosition
constexpr std::uint32_t kParticleRenderY = 0x1Eu;
constexpr std::uint32_t kParticleFlags = 0x3Eu;
constexpr std::uint16_t kParticleScreenSpace = 0x200u;
constexpr std::uint32_t kMaxPieces = 128u + 3800u;
constexpr int kMaxStep = 32;
constexpr const char* kHookId = "rotk.pc-smooth-compose";
constexpr const char* kParticleHookId = "rotk.pc-smooth-particle";
constexpr const char* kPluginId = "rotk.pc.smooth-motion";

struct Position {
    int x = 0;
    int y = 0;
    bool screen = false;  // drawn without the camera (HUD, overlay list)
};

struct Tag {
    std::uint32_t actor = 0;  // 0: not from sprite_composeOam
    Position position;
};

struct State {
    bool enabled = false;
    bool particles = true;  // option "particles"
    bool parallax = true;   // option "parallax"
    // Per shadow buffer.
    std::vector<Tag> tags[2];
    std::uint32_t pieces[2] = {};
    std::unordered_map<std::uint32_t, Position> positions[2];
    bool composing[2] = {};
    std::uint64_t composed[2] = {};  // serial of the finished compose
    int cameraX[2] = {}, cameraY[2] = {};
    std::uint64_t serial = 0;
    // Current sprite_composeOam call.
    std::uint32_t actor = 0;
    Position position;
    // Display.
    int displayed = -1;
    int vblanksShown = 0;  // vblanks the displayed buffer has been on screen, 0 on the commit
    // Interpolation of the frame being drawn.
    bool interpolate = false;
    int bgOffset[4][2] = {};
    std::vector<std::int8_t> objOffset;  // dx, dy per piece
    std::vector<std::uint8_t> extraCopy;
    // BG1 parallax: scroll and camera at the last tick, and scroll change per camera pixel.
    bool parallaxValid = false;
    int parallaxScroll[2] = {}, parallaxCamera[2] = {};
    float parallaxRatio[2] = {};
    std::uint64_t frames = 0, interpolated = 0, movedBg = 0, movedObj = 0;
};

State g;

int shadowBuffer(std::uint32_t shadow)
{
    return shadow == 0x03004340u ? 0 : shadow == 0x03004B80u ? 1 : -1;
}

int composingBuffer()
{
    return shadowBuffer(bus_read_u32(0x03004B40u));  // g_OamShadow
}

std::uint32_t activeCamera()
{
    return kCameraPositions + (bus_read_u8(kActivePlayer) & 1u) * 8u;
}

// Half a step, truncated towards zero so opposite moves (camera vs world) stay symmetric.
int half(int delta)
{
    return delta / 2;
}

bool small(int dx, int dy)
{
    return dx >= -kMaxStep && dx <= kMaxStep && dy >= -kMaxStep && dy <= kMaxStep;
}

int composeOamEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const int buffer = composingBuffer();
    if (buffer < 0) return 0;
    if (!g.composing[buffer]) {
        g.positions[buffer].clear();
        g.composing[buffer] = true;
    }
    const std::uint32_t actor = cpu->R[0];
    Position position;
    position.x = static_cast<std::int32_t>(bus_read_u32(actor + kActorRenderX)) >> 16;
    position.y = static_cast<std::int32_t>(bus_read_u32(actor + kActorRenderY)) >> 16;
    position.screen = (bus_read_u32(actor + kActorFlags) & kActorScreenSpace) ||
                      bus_read_u8(actor + kActorListId) == kListOverlay;
    g.actor = actor;
    g.position = position;
    g.positions[buffer][actor] = position;
    return 0;  // observe only
}

// fx_drawParticle(FxParticle*): its pieces belong to the particle, keyed by its record like an
// actor. Native calls do not keep the caller in LR, so a piece's owner is the latest compose or
// particle entry until the end of the frame.
int drawParticleEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const int buffer = composingBuffer();
    g.actor = 0;
    if (buffer < 0 || !g.particles) return 0;
    if (!g.composing[buffer]) {
        g.positions[buffer].clear();
        g.composing[buffer] = true;
    }
    const std::uint32_t particle = cpu->R[0];
    Position position;
    position.x = static_cast<std::int16_t>(bus_read_u16(particle + kParticleRenderX));
    position.y = static_cast<std::int16_t>(bus_read_u16(particle + kParticleRenderY));
    position.screen = (bus_read_u16(particle + kParticleFlags) & kParticleScreenSpace) != 0;
    g.actor = particle;
    g.position = position;
    g.positions[buffer][particle] = position;
    return 0;
}

// The presented camera moves half way, limited so the world layers stay inside the copied tiles:
// the game copies 31x21 tiles for a 240x160 view, so the fine scroll plus the shift must stay in 0..8.
int cameraShift(int delta, int fine)
{
    int shift = half(delta);
    if (shift < -fine) shift = -fine;
    if (shift > 8 - fine) shift = 8 - fine;
    return shift;
}

// Signed difference of two `bits`-wide wrapping values.
int wrapDelta(int delta, int bits)
{
    const int size = 1 << bits;
    delta &= size - 1;
    return delta >= size / 2 ? delta - size : delta;
}

// BG1 is a parallax pattern whose next scroll is not stored anywhere before the commit. It follows
// the camera at a fixed ratio, learnt from the change between the last two ticks.
void learnParallax(const std::uint8_t* io, int shown)
{
    const int scroll[2] = {io[0x14] | io[0x15] << 8, io[0x16] | io[0x17] << 8};
    const int camera[2] = {g.cameraX[shown], g.cameraY[shown]};
    for (int axis = 0; axis < 2 && g.parallaxValid; ++axis) {
        const int cameraDelta = camera[axis] - g.parallaxCamera[axis];
        if (cameraDelta == 0 || cameraDelta < -kMaxStep || cameraDelta > kMaxStep) continue;
        const float ratio = static_cast<float>(wrapDelta(scroll[axis] - g.parallaxScroll[axis], 9)) / cameraDelta;
        if (ratio >= -2.0f && ratio <= 2.0f) g.parallaxRatio[axis] = ratio;
    }
    for (int axis = 0; axis < 2; ++axis) {
        g.parallaxScroll[axis] = scroll[axis];
        g.parallaxCamera[axis] = camera[axis];
    }
    g.parallaxValid = true;
}

void prepareFrame(const std::uint8_t* io)
{
    ++g.frames;
    g.interpolate = false;
    const int shown = g.displayed;
    if (!g.enabled || shown < 0) return;
    if (g.vblanksShown == 0) learnParallax(io, shown);
    if (g.vblanksShown != 1) return;
    const int next = 1 - shown;
    if (g.composing[next] || g.composed[next] <= g.composed[shown]) return;  // next tick not ready

    for (auto& layer : g.bgOffset) layer[0] = layer[1] = 0;
    int shiftX = 0, shiftY = 0;
    const int cameraDx = g.cameraX[next] - g.cameraX[shown], cameraDy = g.cameraY[next] - g.cameraY[shown];
    const bool cameraFollows = small(cameraDx, cameraDy);
    if (bus_read_u32(kSceneCurrentId) == kSceneGameplay && cameraFollows) {
        // World layers BG3 and BG2 hold the camera's tiles; their scroll is only the fine offset.
        shiftX = cameraShift(cameraDx, (io[0x1C] | io[0x1D] << 8) & 7);
        shiftY = cameraShift(cameraDy, (io[0x1E] | io[0x1F] << 8) & 7);
        g.bgOffset[2][0] = g.bgOffset[3][0] = shiftX;
        g.bgOffset[2][1] = g.bgOffset[3][1] = shiftY;
        if (g.parallax && (io[1] & 0x02u)) {  // BG1 on
            g.bgOffset[1][0] = static_cast<int>(std::lround(g.parallaxRatio[0] * cameraDx * 0.5f));
            g.bgOffset[1][1] = static_cast<int>(std::lround(g.parallaxRatio[1] * cameraDy * 0.5f));
        }
    }

    // Sprite pieces of the displayed buffer: their actor's halfway position, through the presented
    // camera for world actors.
    const std::uint32_t pieces = g.pieces[shown];
    g.objOffset.assign(static_cast<std::size_t>(pieces) * 2u, 0);
    for (std::uint32_t i = 0; i < pieces; ++i) {
        const Tag& tag = g.tags[shown][i];
        if (!tag.actor) continue;
        const auto found = g.positions[next].find(tag.actor);
        if (found == g.positions[next].end() || found->second.screen != tag.position.screen) continue;
        const int dx = found->second.x - tag.position.x, dy = found->second.y - tag.position.y;
        if (!small(dx, dy)) continue;
        int ox = half(dx), oy = half(dy);
        if (!tag.position.screen) {
            if (!cameraFollows) continue;
            ox -= shiftX;
            oy -= shiftY;
        }
        g.objOffset[i * 2] = static_cast<std::int8_t>(ox);
        g.objOffset[i * 2 + 1] = static_cast<std::int8_t>(oy);
    }
    g.interpolate = true;
    ++g.interpolated;
    bool obj = false;
    for (const std::int8_t offset : g.objOffset) obj = obj || offset;
    g.movedBg += shiftX || shiftY;
    g.movedObj += obj;
}

void moveObject(std::uint8_t* entry, int dx, int dy)
{
    std::uint16_t attr0 = static_cast<std::uint16_t>(entry[0] | entry[1] << 8);
    std::uint16_t attr1 = static_cast<std::uint16_t>(entry[2] | entry[3] << 8);
    if ((attr0 & 0x0300u) == 0x0200u) return;  // hidden
    attr0 = static_cast<std::uint16_t>((attr0 & ~0xFFu) | ((attr0 + dy) & 0xFFu));
    attr1 = static_cast<std::uint16_t>((attr1 & ~0x1FFu) | ((attr1 + dx) & 0x1FFu));
    entry[0] = static_cast<std::uint8_t>(attr0);
    entry[1] = static_cast<std::uint8_t>(attr0 >> 8);
    entry[2] = static_cast<std::uint8_t>(attr1);
    entry[3] = static_cast<std::uint8_t>(attr1 >> 8);
}

void presentScanline(std::uint32_t y, std::uint8_t* io, std::uint8_t* oam)
{
    transparencyScanline(y, io, oam);  // first: it replaces whole entries that smoothing then moves
    if (y == 0) prepareFrame(io);
    if (!g.interpolate) return;
    for (unsigned layer = 0; layer < 4; ++layer)
        for (unsigned axis = 0; axis < 2; ++axis) {
            const int offset = g.bgOffset[layer][axis];
            if (!offset) continue;
            const unsigned reg = 0x10u + layer * 4u + axis * 2u;
            const int value = ((io[reg] | io[reg + 1] << 8) + offset) & 0x1FF;
            io[reg] = static_cast<std::uint8_t>(value);
            io[reg + 1] = static_cast<std::uint8_t>(value >> 8);
        }
    const std::uint32_t hardware = g.pieces[g.displayed] < 128u ? g.pieces[g.displayed] : 128u;
    for (std::uint32_t i = 0; i < hardware; ++i) {
        const int dx = g.objOffset[i * 2], dy = g.objOffset[i * 2 + 1];
        if (dx || dy) moveObject(oam + i * 8u, dx, dy);
    }
}

void report()
{
    if (g.frames)
        std::fprintf(stderr, "[rotk] smooth motion: %llu frames, %llu interpolated (%llu moved BG, %llu moved sprites)\n",
                     static_cast<unsigned long long>(g.frames), static_cast<unsigned long long>(g.interpolated),
                     static_cast<unsigned long long>(g.movedBg), static_cast<unsigned long long>(g.movedObj));
}

void enable()
{
    g.enabled = true;
    for (auto& tags : g.tags) tags.assign(kMaxPieces, Tag{});
    gba_mod_set_function_hook_enabled(kHookId, 1);
    gba_mod_set_function_hook_enabled(kParticleHookId, 1);
    // The sprite push and end-frame observers of src/main.cpp feed the tags. With the extra sprite
    // pages off they behave like the stock functions.
    gba_mod_set_function_hook_enabled("rotk.pc-push-oam", 1);
    gba_mod_set_function_hook_enabled("rotk.pc-hide-oam", 1);
    gba_mod_set_function_hook_enabled("rotk.pc-end-frame", 1);
    smoothRefreshPresentHook();
    std::fprintf(stderr, "[rotk] smooth motion on\n");
}

// A boolean sub-option: the environment variable when set ("0" is off), else the package option,
// else on.
bool subOption(const char* env, const char* option)
{
    if (const char* value = std::getenv(env)) return std::string(value) != "0";
    const char* value = gba_mod_option_value("rotk.pc", "smooth-motion", option);
    return !value || std::string(value) != "false";
}

void smoothActivated()
{
    if (std::getenv("ROTK_PC_SMOOTH")) return;
    enable();
    g.particles = subOption("ROTK_PC_SMOOTH_PARTICLES", "particles");
    g.parallax = subOption("ROTK_PC_SMOOTH_PARALLAX", "parallax");
}

}  // namespace

void smoothMotionRegister()
{
    gba_mod_register_function_entry_plugin(kHookId, kComposeOam, 1, composeOamEntry);
    gba_mod_register_function_entry_plugin(kParticleHookId, kDrawParticle, 1, drawParticleEntry);
    gba_mod_register_activation_plugin(kPluginId, smoothActivated);
    std::atexit(report);
}

void smoothMotionApply()
{
    g = State{};
    smoothRefreshPresentHook();
    gba_mod_set_function_hook_enabled(kHookId, 0);
    gba_mod_set_function_hook_enabled(kParticleHookId, 0);
    // ROTK_PC_SMOOTH overrides the package feature (whose activation runs after this).
    if (const char* env = std::getenv("ROTK_PC_SMOOTH"))
        if (std::string(env) != "0") {
            enable();
            g.particles = subOption("ROTK_PC_SMOOTH_PARTICLES", "particles");
            g.parallax = subOption("ROTK_PC_SMOOTH_PARALLAX", "parallax");
        }
}

void smoothRefreshPresentHook()
{
    gba::g_present_scanline_hook = g.enabled || transparencyActive() ? presentScanline : nullptr;
}

bool smoothMotionActive()
{
    return g.enabled;
}

void smoothNotePiece(int buffer, std::uint32_t index)
{
    if (!g.enabled || buffer < 0 || index >= kMaxPieces) return;
    Tag tag;
    if (g.actor) {
        tag.actor = g.actor;
        tag.position = g.position;
    }
    g.tags[buffer][index] = tag;
    if (index == 0 || index + 1u > g.pieces[buffer]) g.pieces[buffer] = index + 1u;
}

void smoothEndFrame(int buffer)
{
    g.actor = 0;
    if (!g.enabled || buffer < 0) return;
    g.composing[buffer] = false;
    const std::uint32_t camera = activeCamera();
    g.cameraX[buffer] = static_cast<std::int32_t>(bus_read_u32(camera));
    g.cameraY[buffer] = static_cast<std::int32_t>(bus_read_u32(camera + 4u));
    g.composed[buffer] = ++g.serial;
}

void smoothSetDisplayedBuffer(int buffer)
{
    if (buffer == g.displayed) ++g.vblanksShown;
    else g.vblanksShown = 0;
    g.displayed = buffer;
}

const std::uint8_t* smoothAdjustExtra(const std::uint8_t* pieces, std::uint32_t count)
{
    if (!g.interpolate || !pieces || count == 0) return pieces;
    g.extraCopy.assign(pieces, pieces + count * 8u);
    const std::uint32_t tagged = g.objOffset.size() / 2u;
    for (std::uint32_t k = 0; k < count && 128u + k < tagged; ++k) {
        const int dx = g.objOffset[(128u + k) * 2], dy = g.objOffset[(128u + k) * 2 + 1];
        if (dx || dy) moveObject(g.extraCopy.data() + k * 8u, dx, dy);
    }
    return g.extraCopy.data();
}

}  // namespace rotk
