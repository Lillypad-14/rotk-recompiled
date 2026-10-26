#include <cstdio>
#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "mod_function_hooks.h"
#include "widescreen.h"

extern "C" unsigned g_ws_active, g_ws_extra_left, g_ws_extra_right;

int main()
{
    gba::GbaBus bus;
    gbarecomp::set_active_bus(&bus);
    gbarecomp::RunOptions options;
    rotk::widescreenRegister(options);
    gba_mod_set_function_hook_enabled("rotk.pc.widescreen-camera", 1);
    g_ws_active = 1;
    g_ws_extra_left = g_ws_extra_right = 22;
    bus.write32(0x030041B0, 1); // gameplay
    bus.write16(0x03005F90, 144); // world width, tiles
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    // gfx_updatePlayerCameraScroll has already stored the unclamped camera in g_CameraPositions
    // (0x030020F8), the tile cameras the builder and screenblock DMA read (0x03005880) and their
    // copies (0x03005FA0), plus both world layers' fine x scroll. The clamp must update all of them
    // for the committing player only, and leave y and the BG3 staging (from 0x03005890) alone.
    for (int margin : {8, 22, 40, 67, 80}) {
      g_ws_extra_left = g_ws_extra_right = margin;
      for (int player : {0, 1}) {
        for (int camera : {0, 440, 912}) {
            const int other = 1 - player;
            for (std::uint32_t base : {0x030020F8u, 0x03005880u, 0x03005FA0u}) {
                bus.write32(base + player * 8, camera);
                bus.write32(base + player * 8 + 4, 300);
                bus.write32(base + other * 8, 0x11111111);
                bus.write32(base + other * 8 + 4, 0x22222222);
            }
            bus.write16(0x03005FB8, camera & 7);
            bus.write16(0x0300211E, camera & 7);
            bus.write32(0x03005890, 0x12345678);  // first BG3 staging entries
            bus.write32(0x03005894, 0x9ABCDEF0);
            ArmCpuState cpu{};
            cpu.R[0] = player;
            cpu.R[14] = 0x0803EC49;
            gba_mod_function_entry(0x0804E0B8, 1, &cpu);
            const int high = 912 - margin;
            const int expected = camera < margin ? margin : camera > high ? high : camera;
            for (std::uint32_t base : {0x030020F8u, 0x03005880u, 0x03005FA0u}) {
                check(bus.read32(base + player * 8) == static_cast<std::uint32_t>(expected), "every camera copy clamped to wide view");
                check(bus.read32(base + player * 8 + 4) == 300, "clamp keeps camera y");
                check(bus.read32(base + other * 8) == 0x11111111 && bus.read32(base + other * 8 + 4) == 0x22222222,
                      "clamp leaves the other player's camera");
            }
            check(bus.read16(0x03005FB8) == (expected & 7) && bus.read16(0x0300211E) == (expected & 7),
                  "both world layers' fine scroll follow the clamp");
            check(bus.read32(0x03005890) == 0x12345678 && bus.read32(0x03005894) == 0x9ABCDEF0,
                  "camera clamp preserves BG3 tile staging entries");
        }
      }
    }
    // Authored margin tiles must refresh at each scanline and frame, even
    // when their tile coordinate is unchanged by a scroll update.
    // The margins continue the tiles in VRAM, so they follow the tile camera the screenblock DMA
    // copies from, even when g_CameraPositions has moved on.
    bus.write8(0x03003F59, 0);
    bus.write32(0x030020F8, 640);
    bus.write32(0x030020FC, 480);
    bus.write32(0x03005880, 0);
    bus.write32(0x03005884, 0);
    bus.write16(0x03005F88, 160);
    bus.write16(0x03005878, 36);
    bus.write32(0x03005FB4, 0x02010000);
    bus.write32(0x03005FBC, 0x02012000);
    bus.write16(0x02010000 + 7 * 2, 1);
    bus.write16(0x02012000 + 32 + 4, 0x2345);
    std::uint8_t io[0x400] = {};
    io[1] = 0x0C;
    gbarecomp::ExtendedViewFrameInfo frame;
    frame.io = io;
    frame.io_size = sizeof(io);
    options.extended_view_init(284, 160);
    options.extended_view_frame(&frame);
    std::uint16_t entry = 0;
    check(gba::g_ws_tilemap_provider(3, 240, 0, &entry) == gba::kWsTilemapReplace && entry == 0x2345,
          "margin resolves the authored tile");
    check(gba::g_ws_tilemap_provider(3, 241, 0, &entry) == gba::kWsTilemapReplace && entry == 0x2345,
          "adjacent margin pixel keeps its tile");
    bus.write16(0x02012000 + 32 + 4, 0x6789);
    gba::g_ws_tilemap_provider(3, 241, 1, &entry);
    check(entry == 0x6789, "tile changes are visible on the next scanline");
    bus.write16(0x02012000 + 32 + 4, 0x3456);
    options.extended_view_frame(&frame);
    gba::g_ws_tilemap_provider(3, 240, 0, &entry);
    check(entry == 0x3456, "new frame invalidates margin lookup cache");
    gbarecomp::set_active_bus(nullptr);
    if (!failures) std::puts("widescreen edge checks passed");
    return failures ? 1 : 0;
}
