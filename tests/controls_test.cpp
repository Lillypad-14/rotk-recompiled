// RotkControlsCheck: both players' devices and bindings on a real HostWindow, headless (SDL dummy
// video) with SDL virtual controllers: migration of the launcher's player 2 keys, Auto assignment,
// press to choose a device, press to bind a key and a controller button, a missing controller's
// keyboard fallback and its return, swapping players, the files the launcher reads, and the in-game
// Controls menu's rows.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "controls_menu.h"
#include "host_window.h"
#include "recomp_runtime_ui.h"
#include "runtime.h"

namespace controls = gbarecomp::controls;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// A virtual Xbox-shaped controller; product ids keep their GUIDs apart.
int attachPad(Uint16 product, const char* name)
{
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = 0x045e;
    desc.product_id = product;
    desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    desc.name = name;
    return SDL_JoystickAttachVirtualEx(&desc);
}

SDL_Joystick* openPad(int device_index)
{
    return SDL_JoystickOpen(device_index);
}

int deviceIndex(SDL_Joystick* joystick)
{
    const SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) return i;
    return -1;
}

void pump(gbarecomp::HostWindow& win, int times = 3)
{
    for (int i = 0; i < times; ++i) win.pump();
}

// Presses and releases a virtual button across pumps, as a player would.
void press(gbarecomp::HostWindow& win, SDL_Joystick* pad, SDL_GameControllerButton button)
{
    SDL_JoystickSetVirtualButton(pad, button, 1);
    pump(win);
    SDL_JoystickSetVirtualButton(pad, button, 0);
    pump(win);
}

void pressKey(gbarecomp::HostWindow& win, SDL_Scancode scancode)
{
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.scancode = scancode;
    e.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
    SDL_PushEvent(&e);
    pump(win, 1);
    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
    pump(win, 1);
}

std::string guidOf(SDL_Joystick* pad)
{
    char buf[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(pad), buf, sizeof(buf));
    return buf;
}

}  // namespace

int main(int, char**)
{
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "rotk_controls_check";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // What a launcher-created keybinds.ini carries: player 2 on player 1's keys.
    std::ofstream(dir / "keybinds.ini") << "[player1]\na = X\nb = Z\n\n[player2]\na = X\nb = Z\nup = Up\n";

    gbarecomp::HostWindow win;
    if (!win.open(1, 240, 160, "controls check")) {
        std::printf("FAIL: window did not open (%s)\n", SDL_GetError());
        return 1;
    }
    win.load_input_config(dir.string().c_str());

    controls::Settings s = gbarecomp::player_controls();
    check(s.version >= 1, "migration ran (ControlsVersion >= 1)");
    check(s.player[1].keys[0] == SDL_SCANCODE_J && s.player[1].keys[6] == SDL_SCANCODE_W,
          "player 2's keys are WASD/J, not the launcher's copy of player 1's");
    check(s.player[0].source == controls::Source::Auto && s.player[1].source == controls::Source::Auto,
          "both players start on Auto");

    // Two controllers: Auto hands them out in order.
    const int first = attachPad(0x02e0, "Pad One");
    SDL_Joystick* one = openPad(first);
    pump(win);
    const int second = attachPad(0x02ea, "Pad Two");
    SDL_Joystick* two = openPad(second);
    pump(win);
    check(one && two && guidOf(one) != guidOf(two), "two virtual controllers with distinct GUIDs");
    check(gbarecomp::connected_controllers().size() == 2, "both controllers connected");
    check(gbarecomp::player_controller(0) == 0 && gbarecomp::player_controller(1) == 1,
          "Auto: first controller to player 1, second to player 2");

    SDL_JoystickSetVirtualButton(two, SDL_CONTROLLER_BUTTON_A, 1);
    pump(win);
    check((win.player_two_buttons() & 1u) != 0, "player 2's controller A presses GBA A");
    const auto events = win.pump();
    check((events.keyinput & 1u) != 0, "player 1 is not pressing A");
    SDL_JoystickSetVirtualButton(two, SDL_CONTROLLER_BUTTON_A, 0);
    pump(win);

    // Player 1's left stick now moves too.
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTX, 30000);
    pump(win);
    check((win.pump().keyinput & (1u << 4)) == 0, "player 1's left stick presses Right");
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTX, 0);
    pump(win);

    // Press to choose: player 1 presses on pad two, the players trade controllers.
    gbarecomp::begin_device_claim(0);
    press(win, two, SDL_CONTROLLER_BUTTON_B);
    s = gbarecomp::player_controls();
    check(s.player[0].source == controls::Source::Gamepad && s.player[0].guid == guidOf(two),
          "press to choose: pad two is player 1's");
    check(gbarecomp::player_controller(0) == 1 && gbarecomp::player_controller(1) == 0,
          "player 2 moves to the free controller");
    check(gbarecomp::device_claim_player() == -1, "claim finished");

    // Press to bind a controller button: player 1's A on X, on pad two only.
    gbarecomp::begin_binding_capture(0, 0, true);
    press(win, one, SDL_CONTROLLER_BUTTON_Y);   // not player 1's controller: ignored
    check(gbarecomp::binding_capture_active(), "another player's controller does not bind");
    press(win, two, SDL_CONTROLLER_BUTTON_X);
    s = gbarecomp::player_controls();
    check(!gbarecomp::binding_capture_active(), "capture finished");
    check(controls::profile_for(s, guidOf(two)).bind[0].code == SDL_CONTROLLER_BUTTON_X,
          "pad two: GBA A on X");
    check(controls::profile_for(s, guidOf(one)).bind[0].code == SDL_CONTROLLER_BUTTON_A,
          "pad one keeps A on A");
    SDL_JoystickSetVirtualButton(two, SDL_CONTROLLER_BUTTON_X, 1);
    pump(win);
    check((win.pump().keyinput & 1u) == 0, "player 1 presses X: GBA A");
    SDL_JoystickSetVirtualButton(two, SDL_CONTROLLER_BUTTON_X, 0);
    pump(win);

    // Conflict: binding B onto X (A's button now) swaps them.
    gbarecomp::begin_binding_capture(0, 1, true);
    press(win, two, SDL_CONTROLLER_BUTTON_X);
    const controls::PadProfile swapped = controls::profile_for(gbarecomp::player_controls(), guidOf(two));
    check(swapped.bind[1].code == SDL_CONTROLLER_BUTTON_X && swapped.bind[0].code == SDL_CONTROLLER_BUTTON_B,
          "binding a used button swaps the two");

    // Press to bind a key, and Escape cancelling one.
    gbarecomp::begin_binding_capture(1, 0, false);
    pressKey(win, SDL_SCANCODE_Q);
    check(gbarecomp::player_controls().player[1].keys[0] == SDL_SCANCODE_Q, "player 2's A key is Q");
    gbarecomp::begin_binding_capture(1, 1, false);
    pressKey(win, SDL_SCANCODE_ESCAPE);
    check(!gbarecomp::binding_capture_active() &&
              gbarecomp::player_controls().player[1].keys[1] == SDL_SCANCODE_K,
          "Escape cancels and keeps the key");

    // Player 1's controller goes missing: keyboard fallback and a notice, then it comes back.
    int noticed = -1;
    bool connected = true;
    while (gbarecomp::take_device_notice(&noticed, &connected)) {}
    SDL_JoystickClose(two);
    SDL_JoystickDetachVirtual(deviceIndex(two) >= 0 ? deviceIndex(two) : second);
    pump(win);
    check(gbarecomp::player_waiting_for_controller(0), "player 1 waits for their controller");
    check(gbarecomp::take_device_notice(&noticed, &connected) && noticed == 0 && !connected,
          "a notice says player 1's controller is missing");
    check(gbarecomp::player_controller(1) == 0, "player 2 keeps pad one");
    const int back = attachPad(0x02ea, "Pad Two");
    two = openPad(back);
    pump(win);
    check(!gbarecomp::player_waiting_for_controller(0) && gbarecomp::player_controller(0) >= 0,
          "pad two is back with player 1");
    check(controls::profile_for(gbarecomp::player_controls(), guidOf(two)).bind[1].code == SDL_CONTROLLER_BUTTON_X,
          "its layout came back with it");

    // Swap players.
    s = gbarecomp::player_controls();
    controls::swap_players(s);
    gbarecomp::set_player_controls(s);
    s = gbarecomp::player_controls();
    check(s.player[1].guid == guidOf(two) && s.player[0].keys[0] == SDL_SCANCODE_Q,
          "swap: player 2 has pad two, player 1 has player 2's keys");

    // The files the launcher reads.
    const std::string config = read(dir / "config.ini");
    const std::string keys = read(dir / "keybinds.ini");
    const std::string input = read(dir / "input.ini");
    check(config.find("SourceP2 = 2") != std::string::npos && config.find("GuidP2 = " + guidOf(two)) != std::string::npos,
          "config.ini [Controller] names player 2's controller");
    check(keys.find("[player1]") != std::string::npos && keys.find("a = Q") != std::string::npos,
          "keybinds.ini has the swapped keys");
    check(input.find("[mapping." + guidOf(two) + "]") != std::string::npos, "input.ini has pad two's layout");

    // The in-game Controls menu (src/controls_menu.cpp), through the hooks run_game hands it.
    // Now: player 1 Auto on pad one, player 2 on pad two (connected second).
    gbarecomp::RunOptions options;
    rotk::controlsMenuRegister(options);
    const auto* items = static_cast<const RecompRuntimeUiItem*>(options.ui_extra_items);
    check(options.ui_extra_item_count == 51, "51 Controls rows (5 on the page, 23 per player below)");
    const RecompRuntimeUiItem* p2Device = nullptr;
    int bindings = 0;
    for (std::size_t i = 0; i < options.ui_extra_item_count; ++i) {
        if (!std::strcmp(items[i].key, "rotk.controls.p2.device")) p2Device = &items[i];
        if (items[i].type == RECOMP_RUNTIME_UI_BINDING) ++bindings;
    }
    check(bindings == 40, "40 press-to-bind rows");
    int value = -1;
    check(options.ui_get("rotk.controls.p1.device", &value) && value == 0, "menu: player 1 on Auto");
    check(options.ui_get("rotk.controls.p2.device", &value) && value == 3, "menu: player 2 on the second controller");
    check(p2Device && std::strstr(p2Device->choices[3], "Pad Two"), "menu: the choice names the controller");
    char text[64] = {};
    check(options.ui_get_text("rotk.controls.p1.key.a", text, sizeof(text)) && !std::strcmp(text, "Q"),
          "menu: player 1's A key shows Q");
    check(options.ui_get_text("rotk.controls.p2.pad.a", text, sizeof(text)) && !std::strcmp(text, "B"),
          "menu: player 2's A shows pad two's B");
    check(options.ui_get_text("rotk.controls.p2.pad.up", text, sizeof(text)) && !std::strcmp(text, "D-pad Up"),
          "menu: readable controller names");
    const auto padTwo = gbarecomp::connected_controllers()[1];
    std::printf("      pad two reports SDL type %d\n", padTwo.type);
    check(options.ui_get_text("rotk.controls.p2.pad.l", text, sizeof(text)) &&
              !std::strcmp(text, controls::pad_style(padTwo.type) == controls::PadStyle::Xbox ? "LB" : "Left bumper"),
          "menu: names follow the controller's type");

    check(options.ui_set("rotk.controls.p1.device", 1) &&
              gbarecomp::player_controls().player[0].source == controls::Source::Keyboard &&
              gbarecomp::player_controller(0) == -1,
          "menu: player 1 to the keyboard");
    check(options.ui_set("rotk.controls.p2.device", 6) && options.ui_get("rotk.controls.p2.device", &value) &&
              value == 7,
          "menu: stepping onto an empty controller slot moves on to None");
    check(options.ui_set("rotk.controls.p2.device", 3) && gbarecomp::player_controller(1) == 1,
          "menu: player 2 back on pad two");

    check(options.ui_set("rotk.controls.p2.deadzone", 30) && options.ui_get("rotk.controls.p2.deadzone", &value) &&
              value == 30 && controls::profile_for(gbarecomp::player_controls(), guidOf(two)).deadzone_pct == 30,
          "menu: pad two's dead zone");
    check(options.ui_action("rotk.controls.p2.pad.select") == 0 && gbarecomp::binding_capture_active(),
          "menu: a binding row starts a capture and leaves the prompt up");
    pressKey(win, SDL_SCANCODE_ESCAPE);
    check(!gbarecomp::binding_capture_active(), "menu: Escape ends it");
    check(options.ui_action("rotk.controls.p2.reset-pad") &&
              controls::profile_for(gbarecomp::player_controls(), guidOf(two)).bind[0].code == SDL_CONTROLLER_BUTTON_A,
          "menu: reset controller");
    check(options.ui_action("rotk.controls.p1.reset-keys") &&
              gbarecomp::player_controls().player[0].keys[0] == SDL_SCANCODE_X,
          "menu: reset player 1's keys");
    check(options.ui_action("rotk.controls.swap") && gbarecomp::player_controller(0) == 1,
          "menu: swap players");

    win.close();
    std::filesystem::remove_all(dir);
    std::printf(g_failures ? "%d failure(s)\n" : "controls check: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
