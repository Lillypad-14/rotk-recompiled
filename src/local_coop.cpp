#include "local_coop.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "multiplayer_session.h"
#include "runtime_arm.h"
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

// The PC features' hooks are process-wide, so they would act on the session's consoles too. The
// presentation features keep per-console state and the engine limits write into each console's own
// host RAM, so both serve the co-op consoles (pcFeaturesStartCoop); every other hook is off while the
// session runs.
struct FeatureHooks {
    void (*present)(std::uint32_t, std::uint8_t*, std::uint8_t*) = nullptr;
    const std::uint8_t* (*extraObjects)(std::uint32_t*) = nullptr;
    int (*romRead32)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;
    void (*dmaObserver)(int, std::uint32_t, std::uint32_t, std::uint32_t) = nullptr;
};

FeatureHooks g_saved;
bool g_enabled = false;  // set by the feature's activation plugin
bool g_linked = false;
int g_console = -1;
std::uint64_t g_linkedAt = 0;  // session frame the consoles linked
int g_cancelFrames = 0;

// actor_checkCollisions (ROM 0x0800CD5C, run from its IWRAM copy at 0x03002D30) first moves the
// collisionPassive actors of g_CollisionCheckQueue to its end by swapping, which does not keep their
// order, then tests pairs in queue order; of two overlapping enemies the earlier one pushes and the
// later one is stunned. Each console also queues a few objects of its own (its own view's effects),
// so the swaps ordered the shared enemies differently on the two consoles, one console stunned the
// other enemy, and the link desynced ("connection lost" after a fight). In co-op the queue is
// partitioned stably first, so the game's swaps leave it as it is and both consoles test the shared
// actors in the same order.
constexpr std::uint32_t kCheckCollisions = 0x03002D30u;
constexpr std::uint32_t kCollisionQueueLiteral = kCheckCollisions + (0x0800D0D0u - 0x0800CD5Cu);
constexpr std::uint32_t kCollisionQueueCount = 0x03002428u;  // g_CollisionCheckQueueCount
constexpr std::uint32_t kActorFlags = 0xE8u;
constexpr std::uint32_t kFlagCollisionPassive = 1u << 17;
constexpr const char* kCollisionOrderHookId = "rotk.pc.coop-collision-order";

int stableCollisionOrder(std::uint32_t, int, ArmCpuState*)
{
    const std::uint32_t queue = bus_read_u32(kCollisionQueueLiteral);  // the engine limits may move it
    const std::uint32_t count = bus_read_u32(kCollisionQueueCount);
    if (count < 2u || count > 4096u) return 0;
    std::vector<std::uint32_t> active, passive;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t actor = bus_read_u32(queue + i * 4u);
        (bus_read_u32(actor + kActorFlags) & kFlagCollisionPassive ? passive : active).push_back(actor);
    }
    std::uint32_t slot = queue;
    for (const auto* group : {&active, &passive})
        for (const std::uint32_t actor : *group) {
            bus_write_u32(slot, actor);
            slot += 4u;
        }
    return 0;  // the game's own pass runs on the ordered queue
}

// system_waitVBlank (0x0802A398) clears bit 0 of g_VBlankFlags, then polls it in the loop at 0x0802A3A8
// (ldrh r1,[r2]; r0 = r3 & r1; beq) until the VBlank handler sets it, instead of halting. Each console
// spent most of its frame there, executing the poll for the rest of the frame. In co-op the session
// halts a console at the loop head while the flag is clear (runtime_add_session_idle_loop): it then
// skips to its next interrupt, and the loop sees the same flag the poll would have.
constexpr std::uint32_t kWaitVBlankLoop = 0x0802A3A8u;
constexpr std::uint32_t kVBlankFlags = 0x03001F10u;

bool waitVBlankSpins()
{
    return (g_cpu.cpsr & 0x20u) != 0 && g_cpu.R[2] == kVBlankFlags && g_cpu.R[3] == 1u &&
           (bus_read_u16(kVBlankFlags) & 1u) == 0;
}

// The link master waits for its partner's message in the loop at 0x08046838 (called from
// multiplayer_tick): with transfer bit 0x20 of g_MpMessageMask set, a pass only reads the mask, the
// vblank counter and the last-received time until the serial IRQ sets bit 0x10 or 300 vblanks pass.
// Halting there leaves the CPU boost's extra instructions to the game instead of the spin.
constexpr std::uint32_t kWaitMessageLoop = 0x08046838u;
constexpr std::uint32_t kMpMessageMask = 0x03006C08u;
constexpr std::uint32_t kMpLastReceived = 0x03006C0Cu;
constexpr std::uint32_t kEngineState = 0x030040B0u;  // EngineState *; vblankCounter at +0x1C
constexpr std::uint32_t kMpTransferPending = 0x20u;
constexpr std::uint32_t kMpMessageReceived = 0x10u;

bool waitMessageSpins()
{
    if ((g_cpu.cpsr & 0x20u) == 0 || g_cpu.R[3] != kMpMessageMask || g_cpu.R[7] != kMpMessageMask ||
        g_cpu.R[12] != kMpLastReceived)
        return false;
    const std::uint32_t mask = bus_read_u32(kMpMessageMask);
    if (!(mask & kMpTransferPending) || (mask & kMpMessageReceived)) return false;
    const auto waited = static_cast<std::int32_t>(bus_read_u32(bus_read_u32(kEngineState) + 0x1Cu) -
                                                  bus_read_u32(kMpLastReceived));
    return waited <= 300;
}

void suspendFeatureHooks()
{
    g_saved = {gba::g_present_scanline_hook, gba::g_extra_obj_provider, gba::g_rom_read32_override, g_dma_observer};
    gba_mod_set_function_hooks_suspended(1);
    gba_mod_set_function_hook_enabled(kCollisionOrderHookId, 1);
    gba_mod_set_function_hook_runs_suspended(kCollisionOrderHookId, 1);
    gbarecomp::runtime_add_session_idle_loop(kWaitVBlankLoop, waitVBlankSpins);
    gbarecomp::runtime_add_session_idle_loop(kWaitMessageLoop, waitMessageSpins);
    pcFeaturesStartCoop();
}

void resumeFeatureHooks()
{
    pcFeaturesEndCoop();
    gbarecomp::runtime_clear_session_idle_loops();
    gba_mod_set_function_hook_runs_suspended(kCollisionOrderHookId, 0);
    gba_mod_set_function_hook_enabled(kCollisionOrderHookId, 0);
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

// Every session is recorded next to the save, so a reported problem can be replayed exactly with
// the test aids: <save>.coop-<time>-1.sram and -2.sram are both consoles' saves at the start, and
// <save>.coop-<time>.inputs.txt both players' buttons as `frame p1 p2` lines counted from the link.
std::FILE* g_recording = nullptr;
std::uint32_t g_recorded = ~0u;

void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes)
{
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
}

void startRecording(const gbarecomp::LocalLinkFrame& link)
{
    if (g_recording) std::fclose(g_recording);
    g_recording = nullptr;
    g_recorded = ~0u;
    if (!link.save_path || std::getenv("ROTK_PC_COOP_TEST_INPUTS")) return;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), ".coop-%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string base = std::string(link.save_path) + stamp;
    writeFile(base + "-1.sram", link.machines[0]->bus.save().sram_bytes());
    writeFile(base + "-2.sram", link.machines[1]->bus.save().sram_bytes());
    g_recording = std::fopen((base + ".inputs.txt").c_str(), "w");
    if (g_recording)
        std::fprintf(g_recording, "# split-screen co-op session: frame p1 p2 (pressed GBA buttons), from the link\n");
}

void record(std::uint64_t frame, std::uint16_t one, std::uint16_t two)
{
    const std::uint32_t buttons = one | static_cast<std::uint32_t>(two) << 16;
    if (!g_recording || buttons == g_recorded) return;
    g_recorded = buttons;
    std::fprintf(g_recording, "%llu %u %u\n", static_cast<unsigned long long>(frame), one, two);
    std::fflush(g_recording);
}

bool frame(gbarecomp::LocalLinkFrame* link)
{
    if (link->frame == 0) {
        startRecording(*link);
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
    record(link->frame - g_linkedAt, link->buttons[0], link->buttons[1]);
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
    if (!script.empty()) {
        // Scripted runs report each console's scene changes and run unpaced with
        // ROTK_PC_COOP_TEST_FAST.
        static const bool fast = std::getenv("ROTK_PC_COOP_TEST_FAST") != nullptr;
        static const char* showFrom = std::getenv("ROTK_PC_COOP_TEST_SHOW_FROM");
        static const char* showEvery = std::getenv("ROTK_PC_COOP_TEST_SHOW_EVERY");
        const std::uint64_t linked = link->frame - g_linkedAt;
        // Explicit offline snapshots for recorded-link investigations. Keep these
        // private: IWRAM contains copied game code as well as session data.
        static const char* dumpFrom = std::getenv("ROTK_PC_COOP_TEST_DUMP_FROM");
        static const char* dumpUntil = std::getenv("ROTK_PC_COOP_TEST_DUMP_UNTIL");
        static std::FILE* snapshots = [] {
            const char* path = std::getenv("ROTK_PC_COOP_TEST_DUMP");
            return path ? std::fopen(path, "wb") : nullptr;
        }();
        if (snapshots && dumpFrom && dumpUntil &&
            linked >= std::strtoull(dumpFrom, nullptr, 10) &&
            linked <= std::strtoull(dumpUntil, nullptr, 10)) {
            for (int i = 0; i < 2; ++i) {
                const std::uint32_t header[] = {static_cast<std::uint32_t>(linked),
                                               static_cast<std::uint32_t>(i)};
                std::fwrite(header, sizeof(header), 1, snapshots);
                std::fwrite(link->machines[i]->bus.iwram_ptr(), 32768, 1, snapshots);
            }
            std::fflush(snapshots);
        }
        link->fast = fast && !(showFrom && linked >= std::strtoull(showFrom, nullptr, 10)) &&
                     !(showEvery && linked % std::strtoull(showEvery, nullptr, 10) == 0);
        static std::uint32_t scenes[2] = {~0u, ~0u};
        const MenuState consoles[2] = {one, two};
        // g_MpLinkState[4..5] (game/include/multiplayer.h): session state and this console's
        // multi-player ID, 0xFF once multiplayer_abortSession has dropped the link.
        // ROTK_PC_COOP_TEST_TRACE=<file>: per frame, each console's scene, frames since a link
        // message last arrived (the game drops the link past 300) and g_MpMessageMask.
        static std::FILE* trace = [] {
            const char* path = std::getenv("ROTK_PC_COOP_TEST_TRACE");
            return path ? std::fopen(path, "w") : nullptr;
        }();
        if (trace) {
            std::fprintf(trace, "%llu", static_cast<unsigned long long>(link->frame - g_linkedAt));
            for (int i = 0; i < 2; ++i) {
                gba::GbaBus& bus = link->machines[i]->bus;
                const std::uint32_t now = bus.read32(bus.read32(0x030040B0u) + 0x1Cu);
                // g_RngGameplayCursor and g_RngGameplayRemaining: the consoles' gameplay random streams.
                std::fprintf(trace, " %u %d %08x ie=%04x sio=%04x rng=%08x/%d", consoles[i].scene,
                             static_cast<int>(now - bus.read32(0x03006C0Cu)), bus.read32(0x03006C08u),
                             bus.read16(0x04000200u), bus.read16(0x04000128u), bus.read32(0x030020CCu),
                             static_cast<int>(bus.read32(0x030020D0u)));
                // Both players' cameras (g_CameraPositions[2]).
                std::fprintf(trace, " cam=%d,%d/%d,%d", static_cast<int>(bus.read32(0x030020F8u)),
                             static_cast<int>(bus.read32(0x030020FCu)), static_cast<int>(bus.read32(0x03002100u)),
                             static_cast<int>(bus.read32(0x03002104u)));
                // Both players' actor positions (g_PlayerGlobals.players[p] actor, coords at +0xAC).
                for (std::uint32_t p = 0; p < 2; ++p) {
                    const std::uint32_t actor = bus.read32(0x03003C00u + p * 0x1ACu);
                    if (actor) std::fprintf(trace, " p%u=%08x,%08x", p, bus.read32(actor + 0xACu), bus.read32(actor + 0xB0u));
                }
            }
            std::fputc('\n', trace);
        }
        static std::uint32_t linkStates[2] = {~0u, ~0u};
        for (int i = 0; i < 2; ++i) {
            const std::uint32_t linkState = link->machines[i]->bus.read16(0x03006D04u);
            if (consoles[i].scene != scenes[i] || linkState != linkStates[i]) {
                std::fprintf(stderr, "[rotk] co-op test: frame %llu console %d scene %u link %04x\n",
                             static_cast<unsigned long long>(link->frame - g_linkedAt), i + 1, consoles[i].scene,
                             linkState);
                scenes[i] = consoles[i].scene;
                linkStates[i] = linkState;
            }
        }
    }
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
    g_console = console;
    pcFeaturesSelectConsole(console);
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
    gba_mod_register_function_entry_plugin(kCollisionOrderHookId, kCheckCollisions, 0, stableCollisionOrder);
    gba_mod_register_activation_plugin("rotk.pc.local-coop", localCoopActivated);
    gba_mod_register_reset_callback(localCoopReset);
    options.local_link = &kLocalLink;
}

}  // namespace rotk
