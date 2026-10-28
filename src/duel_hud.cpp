#include "duel_hud.h"

#include <cstdint>
#include <string>

#include "gba_bus.h"
#include "gba_memory.h"

namespace rotk {
namespace {

// The duel word the mod writes into the host RAM info block every tick (hacks/lib/pc_host.h,
// PcHostInfo.duel, version 3): phase in bits 0-1 (1 fighting, 2 won), the players' scores in bits
// 8-15 and 16-23, the KO target in bits 24-31.
constexpr std::uint32_t kInfoVersion = 0x04u;
constexpr std::uint32_t kInfoDuel = 0x24u;
constexpr std::uint32_t kSceneCurrentId = 0x030041B0u;  // g_SceneCurrent.id
constexpr std::uint32_t kGameFlags = 0x03004144u;       // g_GameFlags
constexpr std::uint32_t kSceneGameplay = 1u;
// GAME_FLAG_WORLD_PARKED and GAME_FLAG_OVERLAY: a menu is over the world.
constexpr std::uint32_t kWorldHidden = (1u << 3) | (1u << 22);
constexpr unsigned kPhaseFight = 1u, kPhaseWon = 2u;

bool g_coop = false;
unsigned g_frame = 0;

struct Color {
    int r, g, b;
};
constexpr Color kPanel{16, 16, 24}, kPlayerColors[2] = {{104, 168, 255}, {255, 104, 88}},
                kGold{255, 216, 96}, kDim{176, 176, 176};

// 3x5 glyphs, one row per byte, bit 2 leftmost.
struct Glyph {
    char c;
    std::uint8_t rows[5];
};
constexpr Glyph kGlyphs[] = {
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 3, 1, 7}},
    {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 2, 2, 2}},
    {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {'P', {7, 5, 7, 4, 4}}, {'F', {7, 4, 6, 4, 4}},
    {'I', {7, 2, 2, 2, 7}}, {'R', {6, 5, 6, 5, 5}}, {'S', {7, 4, 7, 1, 7}}, {'T', {7, 2, 2, 2, 2}},
    {'O', {7, 5, 5, 5, 7}}, {'W', {5, 5, 5, 7, 5}}, {'N', {5, 7, 7, 5, 5}}, {'-', {0, 0, 7, 0, 0}},
};

const Glyph* glyph(char c)
{
    for (const Glyph& g : kGlyphs)
        if (g.c == c) return &g;
    return nullptr;  // a space
}

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

    static int width(const std::string& text, int size) { return static_cast<int>(text.size()) * 4 * size - size; }

    // Text with its top-left at (x, y), each glyph pixel size x size native pixels.
    void text(const std::string& text, int x, int y, int size, Color c) const
    {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const Glyph* g = glyph(text[i]);
            if (!g) continue;
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if (g->rows[row] & (4 >> col))
                        rect(x + (static_cast<int>(i) * 4 + col) * size, y + row * size, size, size, c);
        }
    }
};

}  // namespace

void duelHudSetCoop(bool active)
{
    g_coop = active;
}

bool duelHudActive()
{
    return g_coop;
}

void duelHudDecorate(const gbarecomp::PresentFrame* frame)
{
    if (!g_coop || !frame->bus || frame->console < 0 || frame->console > 1) return;
    gba::GbaBus& bus = *frame->bus;
    if (frame->console == 0) ++g_frame;
    if (bus.read32(gba::kHostRamBase + kInfoVersion) < 3u) return;
    const std::uint32_t duel = bus.read32(gba::kHostRamBase + kInfoDuel);
    const unsigned phase = duel & 3u;
    if ((phase != kPhaseFight && phase != kPhaseWon) || bus.read32(kSceneCurrentId) != kSceneGameplay ||
        (bus.read32(kGameFlags) & kWorldHidden))
        return;
    const int score[2] = {static_cast<int>(duel >> 8 & 0xFFu), static_cast<int>(duel >> 16 & 0xFFu)};
    const int target = static_cast<int>(duel >> 24);
    const Canvas canvas{*frame};

    // Top centre: "P1  3 - 1  P2" with the scores at double size, then the goal or the winner.
    const int centre = static_cast<int>(frame->width / frame->scale) / 2;
    const std::string left = std::to_string(score[0]), right = std::to_string(score[1]);
    const int scoresWidth = Canvas::width(left, 2) + Canvas::width("-", 2) + Canvas::width(right, 2) + 8;
    const int panelWidth = scoresWidth + 2 * (Canvas::width("P1", 1) + 6) + 8;
    const int top = 2, panelLeft = centre - panelWidth / 2;
    canvas.rect(panelLeft, top, panelWidth, 19, kPanel, 200);
    canvas.rect(panelLeft, top + 19, panelWidth, 1, kGold, 160);

    int x = panelLeft + 4;
    canvas.text("P1", x, top + 4, 1, kPlayerColors[0]);
    x += Canvas::width("P1", 1) + 6;
    canvas.text(left, x, top + 2, 2, kPlayerColors[0]);
    x += Canvas::width(left, 2) + 4;
    canvas.text("-", x, top + 2, 2, kDim);
    x += Canvas::width("-", 2) + 4;
    canvas.text(right, x, top + 2, 2, kPlayerColors[1]);
    x += Canvas::width(right, 2) + 6;
    canvas.text("P2", x, top + 4, 1, kPlayerColors[1]);

    // This screen's player is underlined.
    const int mine = frame->console;
    const int underlineX = mine == 0 ? panelLeft + 4 : x;
    canvas.rect(underlineX, top + 10, Canvas::width("P1", 1), 1, kPlayerColors[mine]);

    if (phase == kPhaseWon) {
        const int winner = score[1] > score[0] ? 1 : 0;
        const std::string line = "P" + std::to_string(winner + 1) + " WINS";
        if ((g_frame / 16u) % 2u == 0u)
            canvas.text(line, centre - Canvas::width(line, 1) / 2, top + 13, 1, kGold);
    } else {
        const std::string line = "FIRST TO " + std::to_string(target);
        canvas.text(line, centre - Canvas::width(line, 1) / 2, top + 13, 1, kDim);
    }
}

}  // namespace rotk
