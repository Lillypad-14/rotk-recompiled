// Split-screen feasibility spike: two ROTK machines on one link cable in one process, as a local
// co-op session would run them. Feeds each seat its own input script, measures the host time per
// session frame and writes both screens side by side.
//
//   RotkSplitScreenSpike ROM BIOS FRAMES INPUTS OUT_DIR [SHOT_EVERY [MACHINES]]
//   RotkSplitScreenSpike ROM BIOS play [P1_SAVE [P2_SAVE]]
//
// play opens a window with both screens and runs live at 60 Hz with the local scheduler. Player 1
// uses the normal controls (keybinds.ini, first controller). Player 2 uses a second controller, or
// WASD, J = A, K = B, U = L, I = R, Space = Start, G = Select. Sound is player 1's console. Saves
// are read, never written.
//
// SPLIT_LOCAL_CHUNK=<cycles> uses the local chunk scheduler instead of lockstep slices.
//
// INPUTS lines: `frame p1_buttons p2_buttons`, pressed bits (1 = held, GBA KEYINPUT order),
// held until the next line. Screens go to OUT_DIR/split_<frame>.ppm (480x160).
//
// A link session runs only native code. Code the game copies into RAM is compiled by the runtime
// compiler (GBARECOMP_RAM_OVERLAY_HEAL=1 GBARECOMP_SYNC_OVERLAY_HEAL=1, cache in ./recomp_cache):
// on a missing entry the spike compiles it from the machine's RAM and restarts from cold boot.
#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <thread>

#include "gba_bios.h"
#include "host_window.h"
#include "multiplayer_session.h"
#include "overlay_loader.h"
#include "runtime_bus_bridge.h"
#include "sha1.h"

namespace {

std::vector<std::uint8_t> readFile(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0) throw std::runtime_error(std::string("cannot read ") + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

std::vector<std::array<std::uint16_t, 2>> readInputs(const char* path, unsigned frames)
{
    std::vector<std::array<std::uint16_t, 2>> inputs(frames);
    std::ifstream script(path);
    if (!script) throw std::runtime_error(std::string("cannot read ") + path);
    std::string line;
    while (std::getline(script, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        unsigned frame = 0, p1 = 0, p2 = 0;
        if (!(row >> std::setbase(0) >> frame >> p1 >> p2)) throw std::runtime_error("bad input line: " + line);
        for (unsigned i = frame; i < frames; ++i) inputs[i] = {std::uint16_t(p1), std::uint16_t(p2)};
    }
    return inputs;
}

// Player 2: the second game controller, else the keyboard keys listed above. Pressed bits.
std::uint16_t playerTwoButtons(SDL_GameController* pad)
{
    std::uint16_t held = 0;
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const struct { SDL_Scancode key; std::uint16_t bit; } keyboard[] = {
        {SDL_SCANCODE_J, 1u << 0}, {SDL_SCANCODE_K, 1u << 1}, {SDL_SCANCODE_G, 1u << 2},
        {SDL_SCANCODE_SPACE, 1u << 3}, {SDL_SCANCODE_D, 1u << 4}, {SDL_SCANCODE_A, 1u << 5},
        {SDL_SCANCODE_W, 1u << 6}, {SDL_SCANCODE_S, 1u << 7}, {SDL_SCANCODE_I, 1u << 8},
        {SDL_SCANCODE_U, 1u << 9}};
    for (const auto& k : keyboard)
        if (keys[k.key]) held |= k.bit;
    if (!pad) return held;
    const struct { SDL_GameControllerButton button; std::uint16_t bit; } buttons[] = {
        {SDL_CONTROLLER_BUTTON_A, 1u << 0}, {SDL_CONTROLLER_BUTTON_B, 1u << 1},
        {SDL_CONTROLLER_BUTTON_BACK, 1u << 2}, {SDL_CONTROLLER_BUTTON_START, 1u << 3},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1u << 4}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1u << 5},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 1u << 6}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 1u << 7},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1u << 8}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1u << 9}};
    for (const auto& b : buttons)
        if (SDL_GameControllerGetButton(pad, b.button)) held |= b.bit;
    const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    constexpr int kDead = 16000;
    if (x > kDead) held |= 1u << 4;
    if (x < -kDead) held |= 1u << 5;
    if (y < -kDead) held |= 1u << 6;
    if (y > kDead) held |= 1u << 7;
    return held;
}

// g_SceneCurrent (game/include/scene.h): id, then args[5]. Main menu (scene 5) uses args[1] as the
// cursor row and args[3] as its state (game/include/scene/mainMenu.h).
constexpr std::uint32_t kSceneCurrent = 0x030041B0u;
constexpr std::uint32_t kSceneMainMenu = 5;
constexpr std::uint32_t kMenuPressStart = 0, kMenuIdle = 2, kMenuLinkWait = 3;
constexpr int kEntryMultiplayer = 2;

struct MenuState {
    std::uint32_t scene = 0, cursor = 0, state = 0;
};

MenuState menuState(gba::GbaBus& bus)
{
    return {bus.read32(kSceneCurrent), bus.read32(kSceneCurrent + 8), bus.read32(kSceneCurrent + 16)};
}

// Buttons that walk a console to the main menu and, with `entry` >= 0, choose that row. Each press
// lasts 4 frames with an 8-frame gap, so the game sees a fresh edge.
std::uint16_t steerToMenu(const MenuState& menu, int entry, unsigned frame)
{
    constexpr std::uint16_t kA = 1u << 0, kStart = 1u << 3, kUp = 1u << 6, kDown = 1u << 7;
    if (frame % 12 >= 4) return 0;
    if (menu.scene != kSceneMainMenu || menu.state == kMenuPressStart) return kStart;
    if (menu.state != kMenuIdle || entry < 0) return 0;
    if (menu.cursor < static_cast<std::uint32_t>(entry)) return kDown;
    if (menu.cursor > static_cast<std::uint32_t>(entry)) return kUp;
    return kA;
}

void writeSideBySide(const std::string& path, const std::uint8_t* left, const std::uint8_t* right)
{
    std::ofstream ppm(path, std::ios::binary);
    ppm << "P6\n480 160\n255\n";
    for (unsigned y = 0; y < 160; ++y) {
        ppm.write(reinterpret_cast<const char*>(left + y * 240 * 3), 240 * 3);
        ppm.write(reinterpret_cast<const char*>(right + y * 240 * 3), 240 * 3);
    }
}

int play(const std::vector<std::uint8_t>& rom, const gba::GbaBios& bios, const char* p1Save,
         const char* p2Save)
{
    using namespace gbarecomp;
    const auto romSha1 = gba::sha1(rom.data(), rom.size()).hex();
    overlay_loader_init("recomp_cache", romSha1, &bios);
    GbaSessionConfig config;
    config.links = {{GbaLinkMedium::Cable, {}}};
    for (GbaMachineId id = 0; id < 2; ++id) {
        config.machines.push_back({id, "rotk-split-spike", romSha1});
        config.links[0].machines.push_back(id);
        config.input_machines.push_back(id);
    }
    GbaMultiplayerSession session(config);
    session.set_native_slices(true);
    session.set_local_chunks(1232);
    const char* saves[2] = {p1Save, p2Save};
    for (GbaMachineId id = 0; id < 2; ++id) {
        auto& machine = session.machine(id);
        machine.bus.set_bios(&bios);
        machine.bus.set_rom(rom.data(), rom.size());
        machine.bus.save().configure_sram(32 * 1024);
        if (saves[id]) {
            const auto save = readFile(saves[id]);
            if (!machine.bus.save().load_sram_bytes(save.data(), save.size()))
                throw std::runtime_error(std::string("save does not fit: ") + saves[id]);
        }
    }

    HostWindow window;
    if (!window.open(3, 240, 160, "ROTK split-screen test", nullptr, false, true, false, true))
        throw std::runtime_error("cannot open window");
    // Solo shows player 1 only while player 2's console waits, parked on the main menu. When
    // player 1 waits for a link partner, player 2's console is steered to Multiplayer and the
    // window opens the second screen. It closes again once player 1 is back on the menu.
    bool coop = false;
    unsigned steerFrame = 0;
    // Test aid: SPLIT_P1_INPUTS replays player 1 from an INPUTS script (first column) instead.
    const char* scriptPath = std::getenv("SPLIT_P1_INPUTS");
    const auto script = scriptPath ? readInputs(scriptPath, 100000) : std::vector<std::array<std::uint16_t, 2>>{};
    const auto showScreens = [&](bool both) {
        window.set_surface_size(both ? 480 : 240, 160);
        window.adjust_scale(1);  // resize the OS window to the new surface
        window.adjust_scale(-1);
        window.adjust_scale(1);
        window.adjust_scale(-1);
        window.set_title(both ? "ROTK split-screen test - co-op" : "ROTK split-screen test");
    };
    window.load_input_config(".", false);
    // The window drives player 1 with the first controller; player 2 takes the next one.
    SDL_GameController* padTwo = nullptr;
    for (int i = 0, found = 0; i < SDL_NumJoysticks() && !padTwo; ++i)
        if (SDL_IsGameController(i) && found++ == 1) padTwo = SDL_GameControllerOpen(i);
    std::printf("player 2: %s\n", padTwo ? SDL_GameControllerName(padTwo) : "keyboard (WASD, J/K, Space)");
    std::fflush(stdout);

    std::vector<std::uint8_t> frame(480 * 160 * 3);
    std::vector<std::int16_t> audio(4096 * 2);
    // GBA frame period: 280896 cycles at 16.777216 MHz.
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(280896LL * 1000000000LL / 16777216LL);
    auto next = Clock::now();
    double slowest = 0, total = 0;
    unsigned frames = 0, late = 0;
    for (;;) {
        const auto events = window.pump();
        if (events.quit) break;
        const auto now = Clock::now();
        if (now < next) {
            if (next - now > std::chrono::milliseconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        next = now > next + period * 4 ? now + period : next + period;
        const MenuState one = menuState(session.machine(0).bus), two = menuState(session.machine(1).bus);
        const bool partnerWanted = one.scene == kSceneMainMenu && one.state >= kMenuLinkWait;
        const bool onMenu = one.scene == kSceneMainMenu && one.state < kMenuLinkWait;
        if (!coop && partnerWanted && two.scene == kSceneMainMenu && two.state >= kMenuLinkWait) {
            coop = true;
            showScreens(true);
            std::printf("frame %u: player 2 joined, two screens\n", frames);
            std::fflush(stdout);
        } else if (coop && onMenu) {
            coop = false;
            showScreens(false);
            std::printf("frame %u: back to one screen\n", frames);
            std::fflush(stdout);
        }
        static std::uint32_t lastScene = ~0u;
        if (scriptPath && one.scene != lastScene) {
            std::printf("frame %u: player 1 scene %u, player 2 scene %u\n", frames, one.scene, two.scene);
            lastScene = one.scene;
        }
        // Player 2's console costs nothing while parked on its menu in solo play.
        const bool parked = session.machine_paused(1);
        if (!coop && !partnerWanted && !parked && two.scene == kSceneMainMenu && two.state == kMenuIdle) {
            session.set_machine_paused(1, true);
            std::printf("frame %u: player 2's console parked on its menu\n", frames);
            std::fflush(stdout);
        } else if (parked && partnerWanted) {
            session.set_machine_paused(1, false);
        }
        std::uint16_t secondButtons = 0;
        if (coop) secondButtons = playerTwoButtons(padTwo);
        else secondButtons = steerToMenu(two, partnerWanted ? kEntryMultiplayer : -1, steerFrame++);
        const std::uint16_t firstButtons = frames < script.size() ? script[frames][0]
                                                                  : static_cast<std::uint16_t>(~events.keyinput & 0x3ff);
        const std::array<std::uint16_t, 2> buttons = {firstButtons, secondButtons};
        const auto start = std::chrono::steady_clock::now();
        session.run_frame(std::span<const std::uint16_t>(buttons.data(), 2));
        overlay_drain_ready();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        total += ms; slowest = std::max(slowest, ms); late += ms > 16.74; ++frames;
        if (coop) {
            for (unsigned y = 0; y < 160; ++y) {
                std::memcpy(&frame[y * 480 * 3], session.machine(0).ppu.latched_framebuffer() + y * 240 * 3, 240 * 3);
                std::memcpy(&frame[(y * 480 + 240) * 3], session.machine(1).ppu.latched_framebuffer() + y * 240 * 3, 240 * 3);
            }
            window.present(frame.data(), 480, 160);
        } else {
            window.present(session.machine(0).ppu.latched_framebuffer(), 240, 160);
        }
        std::uint32_t rate = 0;
        while (const auto n = session.machine(0).bus.audio().drain_sample_block_stereo(audio.data(), 4096, rate))
            window.push_audio_block_stereo(audio.data(), n, rate);
        session.machine(1).bus.audio().discard_output();
        if (frames % 600 == 0) {
            std::printf("frames %u: mean %.2f ms, slowest %.2f ms, over budget %u\n", frames, total / 600, slowest, late);
            std::fflush(stdout);
            total = slowest = 0; late = 0;
        }
    }
    if (padTwo) SDL_GameControllerClose(padTwo);
    overlay_loader_shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[3]) == "play") {
        try {
#if defined(_WIN32)
            _putenv_s("GBARECOMP_SESSION_INTERPRETER", "1");
#else
            setenv("GBARECOMP_SESSION_INTERPRETER", "1", 1);
#endif
            const auto rom = readFile(argv[1]);
            gba::GbaBios bios;
            std::string error;
            if (!bios.load_from_file(argv[2], gba::GbaBios::kExpectedSha1, &error)) throw std::runtime_error(error);
            return play(rom, bios, argc > 4 ? argv[4] : nullptr, argc > 5 ? argv[5] : (argc > 4 ? argv[4] : nullptr));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "split spike: %s\n", e.what());
            return 1;
        }
    }
    if (argc < 6) {
        std::fprintf(stderr, "usage: RotkSplitScreenSpike ROM BIOS FRAMES INPUTS OUT_DIR [SHOT_EVERY]\n");
        return 2;
    }
    try {
        using namespace gbarecomp;
        const auto rom = readFile(argv[1]);
        gba::GbaBios bios;
        std::string error;
        if (!bios.load_from_file(argv[2], gba::GbaBios::kExpectedSha1, &error)) throw std::runtime_error(error);
        const unsigned frames = static_cast<unsigned>(std::stoul(argv[3]));
        const auto inputs = readInputs(argv[4], frames);
        const std::string out = argv[5];
        const unsigned shotEvery = argc > 6 ? static_cast<unsigned>(std::stoul(argv[6])) : 300;
        const unsigned machines = argc > 7 ? static_cast<unsigned>(std::stoul(argv[7])) : 2;

        const auto romSha1 = gba::sha1(rom.data(), rom.size()).hex();
        overlay_loader_init("recomp_cache", romSha1, &bios);
        for (unsigned attempt = 1;; ++attempt) {
        GbaSessionConfig config;
        config.links = {{GbaLinkMedium::Cable, {}}};
        for (GbaMachineId id = 0; id < machines; ++id) {
            config.machines.push_back({id, "rotk-split-spike", romSha1});
            config.links[0].machines.push_back(id);
            config.input_machines.push_back(id);
        }
        if (machines == 1) config.links.clear();
        GbaMultiplayerSession session(config);
        session.set_native_slices(true);
        if (const char* chunk = std::getenv("SPLIT_LOCAL_CHUNK")) session.set_local_chunks(std::stoul(chunk));
        for (GbaMachineId id = 0; id < machines; ++id) {
            auto& machine = session.machine(id);
            machine.bus.set_bios(&bios);
            machine.bus.set_rom(rom.data(), rom.size());
            machine.bus.save().configure_sram(32 * 1024);
            // SPLIT_SAVE_P1 / SPLIT_SAVE_P2: SRAM images for each console.
            if (const char* path = std::getenv(id == 0 ? "SPLIT_SAVE_P1" : "SPLIT_SAVE_P2")) {
                const auto save = readFile(path);
                if (!machine.bus.save().load_sram_bytes(save.data(), save.size()))
                    throw std::runtime_error(std::string("save does not fit: ") + path);
            }
        }

        std::vector<double> frameMs;
        frameMs.reserve(frames);
        bool restart = false;
        for (unsigned frame = 0; frame < frames && !restart; ++frame) {
            const auto start = std::chrono::steady_clock::now();
            try {
                session.run_frame(std::span<const std::uint16_t>(inputs[frame].data(), machines));
            } catch (const std::logic_error& missing) {
                const std::string text = missing.what();
                const auto at = text.find("missing: ");
                if (at == std::string::npos || attempt > 200) throw;
                const std::uint32_t pc = std::stoul(text.substr(at + 9, 8), nullptr, 16);
                const bool thumb = text.find("Thumb") != std::string::npos;
                bool compiled = false;
                for (GbaMachineId id = 0; id < machines && !compiled; ++id) {
                    set_active_bus(&session.machine(id).bus);
                    compiled = overlay_request_compile(pc, thumb);
                }
                set_active_bus(nullptr);
                std::printf("attempt %u: frame %u missing %08x %s -> %s\n", attempt, frame + 1, pc,
                            thumb ? "Thumb" : "ARM", compiled ? "compiled, restarting" : "COMPILE FAILED");
                std::fflush(stdout);
                if (!compiled) throw;
                restart = true;
                continue;
            }
            session.discard_audio_output();
            overlay_drain_ready();  // install functions the runtime compiler finished
            frameMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if ((frame + 1) % shotEvery == 0 || frame + 1 == frames)
                writeSideBySide(out + "/split_" + std::to_string(frame + 1) + ".ppm",
                                session.machine(0).ppu.latched_framebuffer(),
                                session.machine(machines - 1).ppu.latched_framebuffer());
        }

        if (restart) continue;
        // Per-window summary: a 60 Hz session needs every frame under 16.7 ms.
        constexpr unsigned kWindow = 600;
        for (unsigned begin = 0; begin < frameMs.size(); begin += kWindow) {
            std::vector<double> window(frameMs.begin() + begin,
                                       frameMs.begin() + std::min<std::size_t>(begin + kWindow, frameMs.size()));
            std::sort(window.begin(), window.end());
            double sum = 0;
            for (double ms : window) sum += ms;
            const auto over = std::count_if(window.begin(), window.end(), [](double ms) { return ms > 16.74; });
            // The median holds up when other programs preempt a few frames; the mean does not.
            std::printf("frames %5u-%5u  mean %6.2f ms  p50 %6.2f  p95 %6.2f  max %6.2f  over-budget %zu\n", begin + 1,
                        begin + unsigned(window.size()), sum / window.size(), window[window.size() / 2],
                        window[window.size() * 95 / 100],
                        window.back(), static_cast<std::size_t>(over));
        }
        overlay_loader_shutdown();
        return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "split spike: %s\n", e.what());
        return 1;
    }
}
