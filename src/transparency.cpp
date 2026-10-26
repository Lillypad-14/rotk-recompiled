#include "transparency.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "smooth_motion.h"

namespace rotk {
namespace {

// Each OAM shadow buffer has two pages and the VBlank handler copies the other page on every
// vblank. sprite_composeOam pushes every piece to both pages and, for actors with sprite flag 0x20,
// calls sprite_hideOamEntry, which hides the piece on page 1 only: the piece shows on every other
// vblank. A piece visible on page 0 and hidden on page 1 is such a see-through piece.

constexpr std::uint32_t kOamShadowA = 0x03004340u;
constexpr std::uint32_t kOamShadowB = 0x03004B80u;
constexpr std::uint32_t kPageBytes = 0x400u;
constexpr std::uint32_t kHardwareObjects = 128u;
constexpr unsigned kBldCnt = 0x50u;
constexpr unsigned kBldAlpha = 0x52u;
constexpr const char* kPluginId = "rotk.pc.transparency";

struct State {
    bool enabled = false;
    int strength = 6;  // sprite weight EVA in 16ths; the layer below gets 16 - strength
    int buffer = -1;
    int page = 0;
    std::vector<std::uint32_t> pieces;  // see-through hardware pieces of the displayed buffer
    std::uint8_t entries[kHardwareObjects][8] = {};  // their page 0 entries
    std::vector<std::uint8_t> extraCopy;
    bool extraSeeThrough = false;  // the extra pieces drawn last had see-through ones
    std::uint64_t frames = 0, framesWithPieces = 0;
};

State g;

bool hidden(const std::uint8_t* entry)
{
    return (entry[1] & 0x03u) == 0x02u;  // attr0: affine off, disable on
}

bool plainMode(const std::uint8_t* entry)
{
    return (entry[1] & 0x0Cu) == 0;  // attr0 bits 10-11: normal (not semi-transparent or window)
}

bool seeThrough(const std::uint8_t* page0, const std::uint8_t* page1)
{
    return !hidden(page0) && hidden(page1) && plainMode(page0);
}

void makeSemiTransparent(std::uint8_t* dst, const std::uint8_t* page0)
{
    std::memcpy(dst, page0, 6);
    dst[1] = static_cast<std::uint8_t>((dst[1] & ~0x0Cu) | 0x04u);
}

void prepareFrame()
{
    ++g.frames;
    g.pieces.clear();
    if (g.buffer < 0) return;
    const std::uint8_t* iwram = gbarecomp::active_bus()->iwram_ptr();
    const std::uint32_t base = (g.buffer == 0 ? kOamShadowA : kOamShadowB) & 0x7FFFu;
    const std::uint8_t* page0 = iwram + base;
    const std::uint8_t* page1 = page0 + kPageBytes;
    for (std::uint32_t i = 0; i < kHardwareObjects; ++i) {
        if (!seeThrough(page0 + i * 8u, page1 + i * 8u)) continue;
        std::memcpy(g.entries[i], page0 + i * 8u, 8);
        g.pieces.push_back(i);
    }
    g.framesWithPieces += !g.pieces.empty();
    static const bool debug = std::getenv("ROTK_PC_TRANSPARENCY_DEBUG") != nullptr;
    if (debug && !g.pieces.empty() && g.frames % 600 == 0)
        for (const std::uint32_t i : g.pieces) {
            const std::uint8_t* e0 = page0 + i * 8u;
            const std::uint8_t* e1 = page1 + i * 8u;
            std::fprintf(stderr, "[rotk] transparency frame %llu buffer %d page %d piece %u: %04x %04x %04x / %04x\n",
                         static_cast<unsigned long long>(g.frames), g.buffer, g.page, i, e0[0] | e0[1] << 8,
                         e0[2] | e0[3] << 8, e0[4] | e0[5] << 8, e1[0] | e1[1] << 8);
        }
}

void report()
{
    if (g.frames)
        std::fprintf(stderr, "[rotk] transparency: %llu frames, %llu with see-through pieces\n",
                     static_cast<unsigned long long>(g.frames), static_cast<unsigned long long>(g.framesWithPieces));
}

void enable()
{
    g.enabled = true;
    smoothRefreshPresentHook();
    std::fprintf(stderr, "[rotk] real transparency on\n");
}

// Option "strength" (percent of the sprite in the mix), or ROTK_PC_TRANSPARENCY_STRENGTH.
int strengthSetting()
{
    const char* value = std::getenv("ROTK_PC_TRANSPARENCY_STRENGTH");
    if (!value) value = gba_mod_option_value("rotk.pc", "transparency", "strength");
    const int percent = value ? std::atoi(value) : 35;
    const int eva = (percent * 16 + 50) / 100;
    return eva < 1 ? 1 : eva > 15 ? 15 : eva;
}

void transparencyActivated()
{
    if (std::getenv("ROTK_PC_TRANSPARENCY")) return;
    enable();
    g.strength = strengthSetting();
}

}  // namespace

void transparencyRegister()
{
    gba_mod_register_activation_plugin(kPluginId, transparencyActivated);
    std::atexit(report);
}

void transparencyApply()
{
    g = State{};
    if (const char* env = std::getenv("ROTK_PC_TRANSPARENCY"))
        if (std::string(env) != "0") {
            enable();
            g.strength = strengthSetting();
        }
    smoothRefreshPresentHook();
}

bool transparencyActive()
{
    return g.enabled;
}

void transparencySetDisplayed(int buffer, int page)
{
    g.buffer = buffer;
    g.page = page;
}

void transparencyScanline(std::uint32_t y, std::uint8_t* io, std::uint8_t* oam)
{
    if (!g.enabled) return;
    if (y == 0) prepareFrame();
    if (g.pieces.empty() && !g.extraSeeThrough) return;
    for (const std::uint32_t i : g.pieces) makeSemiTransparent(oam + i * 8u, g.entries[i]);
    // Semi-transparent sprites blend with the second targets at BLDALPHA. Keep the game's own alpha
    // blend when it runs one; otherwise mix the sprite at the chosen strength over every layer.
    if (((io[kBldCnt] >> 6) & 3u) != 1u) {
        io[kBldCnt + 1] = static_cast<std::uint8_t>(io[kBldCnt + 1] | 0x3Fu);
        io[kBldAlpha] = static_cast<std::uint8_t>(g.strength);
        io[kBldAlpha + 1] = static_cast<std::uint8_t>(16 - g.strength);
    }
}

const std::uint8_t* transparencyAdjustExtra(const std::uint8_t* pieces, const std::uint8_t* page0,
                                            const std::uint8_t* page1, std::uint32_t count)
{
    if (!g.enabled || !pieces || !page0 || !page1 || count == 0) {
        g.extraSeeThrough = false;
        return pieces;
    }
    bool any = false;
    for (std::uint32_t k = 0; k < count && !any; ++k) any = seeThrough(page0 + k * 8u, page1 + k * 8u);
    g.extraSeeThrough = any;
    if (!any) return pieces;
    g.extraCopy.assign(pieces, pieces + count * 8u);
    for (std::uint32_t k = 0; k < count; ++k)
        if (seeThrough(page0 + k * 8u, page1 + k * 8u))
            makeSemiTransparent(g.extraCopy.data() + k * 8u, page0 + k * 8u);
    return g.extraCopy.data();
}

}  // namespace rotk
