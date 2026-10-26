#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "gba_ppu.h"

namespace {
std::uint32_t seed = 0x65fa21;
std::uint32_t randomWord() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
void put16(std::uint8_t* p, unsigned v) { p[0] = v; p[1] = v >> 8; }
int margin(int, int hx, int y, std::uint16_t* entry) {
    // Deliberately vary within a tile: the optimized center path must not
    // coalesce calls to an arbitrary authored-margin provider.
    *entry = static_cast<std::uint16_t>((hx + 1024 + y * 7) ^ 0x9400);
    return gba::kWsTilemapReplace;
}
int remap(int, int x, int, int* hx) { *hx = x / 2; return 1; }
}

int main() {
    gba::GbaPpu ppu;
    std::array<std::uint8_t, 0x400> io{}, oam{}, pal{};
    std::vector<std::uint8_t> vram(0x18000), pixels(gba::GbaPpu::kMaxFramebufferBytes);
    std::uint64_t hash = 14695981039346656037ull;
    double renderMs = 0;
    unsigned cases = 0;
    for (unsigned variant = 0; variant < 64; ++variant) {
        for (auto& v : vram) v = randomWord();
        for (auto& v : pal) v = randomWord();
        for (auto& v : io) v = randomWord();
        for (unsigned i = 0; i < 128; ++i) put16(oam.data() + i * 8, 0x0200);
        unsigned dispcnt = 0x0f00 | ((variant & 1) ? 0x2000 : 0);
        for (unsigned bg = 0; bg < 4; ++bg) {
            put16(io.data() + 8 + bg * 2, randomWord());
            put16(io.data() + 0x10 + bg * 4, randomWord() & 511);
            put16(io.data() + 0x12 + bg * 4, randomWord() & 511);
        }
        gba::g_ws_tilemap_provider = variant & 2 ? margin : nullptr;
        gba::g_ws_bg_x_provider = variant & 4 ? remap : nullptr;
        gba::g_ws_bg_x_provider_layers = 1;
        gba::g_ws_authored_margin_layers = variant & 8 ? 1 : 0;
        for (unsigned width : {256, 284, 320, 374, 400}) {
            ppu.set_view_margins((width - 240) / 2, (width - 240) / 2, 0, 0);
            const auto start = std::chrono::steady_clock::now();
            ppu.render(pixels.data(), dispcnt, io.data(), vram.data(), oam.data(), pal.data());
            renderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            for (std::size_t i = 0; i < width * 160 * 3; ++i) { hash ^= pixels[i]; hash *= 1099511628211ull; }
            ++cases;
        }
    }
    std::printf("cases=%u hash=%016llx render_ms=%.3f\n", cases,
                static_cast<unsigned long long>(hash), renderMs);
    // Captured from the unmodified per-pixel wide renderer before the span
    // optimization. This is a differential regression, not a speed-only test.
    if (hash != 0xce715c1d44e9a64aull) {
        std::fprintf(stderr, "wide-render pixels differ from the pre-optimization reference\n");
        return 1;
    }
    return 0;
}
