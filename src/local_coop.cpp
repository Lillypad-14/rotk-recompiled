#include "local_coop.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "multiplayer_session.h"
#include "runtime_bus_bridge.h"
#include "widescreen.h"

extern "C" void (*g_dma_observer)(int channel, std::uint32_t src, std::uint32_t dst, std::uint32_t bytes);

namespace rotk {
namespace {

// g_SceneCurrent (game/include/scene.h): id, then args[5]. The main menu (scene 5) keeps its cursor
// row in args[1] and its state in args[3] (game/include/scene/mainMenu.h).
constexpr std::uint32_t kSceneCurrent = 0x030041B0u;
constexpr std::uint32_t kSceneMainMenu = 5;
constexpr std::uint32_t kMenuPressStart = 0, kMenuIdle = 2, kMenuLinkWait = 3;
constexpr std::uint32_t kEntryMultiplayer = 2;
constexpr std::uint16_t kA = 1u << 0, kB = 1u << 1, kStart = 1u << 3, kUp = 1u << 6, kDown = 1u << 7;
// Frames to reach both consoles' link wait before giving up.
constexpr std::uint64_t kBootFrames = 3000;
// Frames B is held on the solo console to leave its own link wait.
constexpr int kCancelFrames = 6;

struct MenuState {
    std::uint32_t scene = 0, cursor = 0, state = 0;
};

MenuState menuState(gba::GbaBus& bus)
{
    return {bus.read32(kSceneCurrent), bus.read32(kSceneCurrent + 8), bus.read32(kSceneCurrent + 16)};
}

bool waitingForPartner(const MenuState& menu)
{
    return menu.scene == kSceneMainMenu && menu.state >= kMenuLinkWait;
}

// Buttons that walk a console to the main menu and choose Multiplayer. Each press lasts 4 frames
// with an 8-frame gap, so the game sees a fresh edge.
std::uint16_t steerToMultiplayer(const MenuState& menu, std::uint64_t frame)
{
    if (frame % 12 >= 4) return 0;
    if (menu.scene != kSceneMainMenu || menu.state == kMenuPressStart) return kStart;
    if (menu.state != kMenuIdle) return 0;
    if (menu.cursor < kEntryMultiplayer) return kDown;
    if (menu.cursor > kEntryMultiplayer) return kUp;
    return kA;
}

// The PC features' hooks are process-wide, so they would act on the session's consoles too. Only
// features with per-console state serve the co-op consoles (widescreen); every other hook is off
// while the session runs.
struct FeatureHooks {
    void (*present)(std::uint32_t, std::uint8_t*, std::uint8_t*) = nullptr;
    const std::uint8_t* (*extraObjects)(std::uint32_t*) = nullptr;
    int (*romRead32)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;
    void (*dmaObserver)(int, std::uint32_t, std::uint32_t, std::uint32_t) = nullptr;
};

FeatureHooks g_saved;
bool g_enabled = false;  // set by the feature's activation plugin
bool g_linked = false;
std::uint64_t g_linkedAt = 0;  // session frame the consoles linked
int g_cancelFrames = 0;

void coopObserveDma(int, std::uint32_t source, std::uint32_t destination, std::uint32_t)
{
    widescreenObserveDma(source, destination);
}

void suspendFeatureHooks()
{
    g_saved = {gba::g_present_scanline_hook, gba::g_extra_obj_provider, gba::g_rom_read32_override, g_dma_observer};
    gba::g_present_scanline_hook = nullptr;
    gba::g_extra_obj_provider = nullptr;
    gba::g_rom_read32_override = nullptr;
    g_dma_observer = coopObserveDma;
    gba_mod_set_function_hooks_suspended(1);
    widescreenStartCoop();
}

void resumeFeatureHooks()
{
    widescreenEndCoop();
    gba_mod_set_function_hooks_suspended(0);
    gba::g_present_scanline_hook = g_saved.present;
    gba::g_extra_obj_provider = g_saved.extraObjects;
    gba::g_rom_read32_override = g_saved.romRead32;
    g_dma_observer = g_saved.dmaObserver;
}

bool wanted()
{
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    if (g_cancelFrames > 0) {
        if (--g_cancelFrames == 0) bus->io().set_synthesized_keyinput(0x3FFu);
        return false;
    }
    if (!g_enabled || !waitingForPartner(menuState(*bus))) return false;
    suspendFeatureHooks();
    g_linked = false;
    return true;
}

// g_SaveGlobals (game/include/save.h) is the first 0x40 bytes of the save: its halfwords sum to 0,
// and optionsFlags holds OPTION_FLAG_MUSIC_ENABLED.
constexpr std::uint32_t kSaveGlobalsBytes = 0x40, kOptionsFlags = 0x30, kChecksum = 0x3E;
constexpr std::uint8_t kMusicEnabled = 1u << 1;

// Console 2 plays its sound effects into player 1's sound but not the music, which would double.
// The game reads the music flag from the save at boot, so it is cleared there before console 2
// runs, keeping the checksum valid.
void muteMusic(gba::GbaBus& bus)
{
    const auto save = bus.save().sram_bytes();
    if (save.size() < kSaveGlobalsBytes || !(save[kOptionsFlags] & kMusicEnabled)) return;
    std::uint16_t sum = 0;
    for (std::uint32_t i = 0; i < kSaveGlobalsBytes; i += 2) sum += save[i] | save[i + 1] << 8;
    if (sum != 0) return;  // no valid globals yet (a blank save)
    const std::uint16_t checksum = (save[kChecksum] | save[kChecksum + 1] << 8) + kMusicEnabled;
    bus.save().sram_write(kOptionsFlags, save[kOptionsFlags] & ~kMusicEnabled);
    bus.save().sram_write(kChecksum, checksum & 0xFF);
    bus.save().sram_write(kChecksum + 1, checksum >> 8);
}

bool frame(gbarecomp::LocalLinkFrame* link)
{
    if (link->frame == 0) {
        muteMusic(link->machines[1]->bus);
        link->partner_audio = true;
    }
    const MenuState one = menuState(link->machines[0]->bus), two = menuState(link->machines[1]->bus);
    if (!g_linked) {
        // Both fresh consoles boot unshown and walk into Multiplayer, as the player already chose.
        link->fast = true;
        link->buttons[0] = steerToMultiplayer(one, link->frame);
        link->buttons[1] = steerToMultiplayer(two, link->frame);
        g_linked = waitingForPartner(one) && waitingForPartner(two);
        if (g_linked) {
            g_linkedAt = link->frame;
            std::fprintf(stderr, "[rotk] split-screen co-op: consoles linked after %llu frames\n",
                         static_cast<unsigned long long>(link->frame));
        }
        return link->frame < kBootFrames;
    }
    link->screens = 2;
    link->buttons[0] = link->players[0];
    link->buttons[1] = link->players[1];
    // Test aid: ROTK_PC_COOP_TEST_INPUTS=<file> plays both players from lines `frame p1 p2` (frames
    // after the consoles link, pressed GBA button bits, held until the next line).
    static const std::vector<std::array<std::uint32_t, 3>> script = [] {
        std::vector<std::array<std::uint32_t, 3>> rows;
        if (const char* path = std::getenv("ROTK_PC_COOP_TEST_INPUTS"))
            if (std::FILE* file = std::fopen(path, "r")) {
                char line[128];
                std::array<std::uint32_t, 3> row{};
                while (std::fgets(line, sizeof(line), file))
                    if (line[0] != '#' && std::sscanf(line, "%u %u %u", &row[0], &row[1], &row[2]) == 3)
                        rows.push_back(row);
                std::fclose(file);
            }
        return rows;
    }();
    for (const auto& row : script)
        if (row[0] <= link->frame - g_linkedAt) {
            link->buttons[0] = static_cast<std::uint16_t>(row[1]);
            link->buttons[1] = static_cast<std::uint16_t>(row[2]);
        }
    // Test aid: ROTK_PC_COOP_TEST_EXIT_AFTER=N ends the session N frames after the consoles link.
    static const char* exitAfter = std::getenv("ROTK_PC_COOP_TEST_EXIT_AFTER");
    if (exitAfter && link->frame - g_linkedAt >= std::strtoull(exitAfter, nullptr, 10)) {
        // g_SfxMusic.state (game/include/sfx.h): 0 stopped, 1 playing, 2 paused.
        std::fprintf(stderr, "[rotk] split-screen co-op test exit: music state %u / %u\n",
                     link->machines[0]->bus.read32(0x02001648u), link->machines[1]->bus.read32(0x02001648u));
        return false;
    }
    // Back on the main menu (cancelled or co-op finished): the session ends.
    return !(one.scene == kSceneMainMenu && one.state < kMenuLinkWait);
}

void finished()
{
    resumeFeatureHooks();
    // The solo console is still waiting for a link partner. Its prompt cancels with B.
    if (gba::GbaBus* bus = gbarecomp::active_bus()) {
        bus->io().set_synthesized_keyinput(static_cast<std::uint16_t>(0x3FFu & ~kB));
        g_cancelFrames = kCancelFrames;
    }
}

// Player 2 keeps a save of their own unless the "player2-save" option says to use a copy of
// player 1's each session.
bool partnerKeepsSave()
{
    const char* option = gba_mod_option_value("rotk.pc", "local-coop", "player2-save");
    return !option || std::strcmp(option, "copy") != 0;
}

bool shareView()
{
    return widescreenEnabled();
}

void bindConsole(int console)
{
    widescreenSelectConsole(console);
}

const gbarecomp::LocalLinkPlay kLocalLink{wanted, frame, finished, partnerKeepsSave, shareView, bindConsole};

void localCoopActivated()
{
    g_enabled = true;
}

void localCoopReset()
{
    g_enabled = false;
}

}  // namespace

void localCoopRegister(gbarecomp::RunOptions& options)
{
    gba_mod_register_activation_plugin("rotk.pc.local-coop", localCoopActivated);
    gba_mod_register_reset_callback(localCoopReset);
    options.local_link = &kLocalLink;
}

}  // namespace rotk
