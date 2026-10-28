#include "controls_menu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mod_runtime.h"
#include "recomp_runtime_ui.h"

namespace rotk {
namespace {

namespace controls = gbarecomp::controls;

// ---- the old player 2 options -------------------------------------------------------------------

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "local-coop";

// Option id suffix and default per GBA button, in KEYINPUT order (mods/rotk.pc.manifest.toml.in).
struct OldButton {
    const char* id;
    const char* key;
    const char* pad;
};
constexpr OldButton kOldButtons[controls::kButtons] = {
    {"a", "J", "a"}, {"b", "K", "b"}, {"select", "G", "back"}, {"start", "Space", "start"},
    {"right", "D", "dpright"}, {"left", "A", "dpleft"}, {"up", "W", "dpup"}, {"down", "S", "dpdown"},
    {"r", "I", "rightshoulder"}, {"l", "U", "leftshoulder"},
};

const char* oldValue(const OldButton& button, bool pad)
{
    const std::string id = std::string(pad ? "p2-pad-" : "p2-key-") + button.id;
    const char* chosen = gba_mod_option_value(kPackageId, kFeatureId, id.c_str());
    return chosen ? chosen : pad ? button.pad : button.key;
}

// ---- the menu -----------------------------------------------------------------------------------

// GBA buttons in the order a pad groups them (the launcher's order), as KEYINPUT bits.
constexpr int kOrder[controls::kButtons] = {6, 7, 5, 4, 0, 1, 9, 8, 3, 2};

// Device choices: Auto, Keyboard, up to five connected controllers, None.
constexpr int kDeviceAuto = 0;
constexpr int kDeviceKeyboard = 1;
constexpr int kFirstPad = 2;
constexpr int kPadSlots = 5;
constexpr int kDeviceNone = kFirstPad + kPadSlots;
constexpr int kDeviceCount = kDeviceNone + 1;

struct PlayerRows {
    char choiceText[kDeviceCount][80];
    const char* choices[kDeviceCount];
    char padPageNote[160];   // which controller the controller page edits
};
PlayerRows g_rows[2];

std::vector<std::string> g_strings;   // keys, sections, labels: stable for run_game()'s lifetime
std::vector<RecompRuntimeUiItem> g_items;

const char* keep(std::string text)
{
    g_strings.push_back(std::move(text));
    return g_strings.back().c_str();
}

// Parses "rotk.controls.p<n>.<rest>": the player, and the rest.
bool parseKey(const char* key, int& player, std::string& rest)
{
    constexpr const char prefix[] = "rotk.controls.p";
    if (std::strncmp(key, prefix, sizeof(prefix) - 1) != 0) return false;
    const char* p = key + sizeof(prefix) - 1;
    if ((p[0] != '1' && p[0] != '2') || p[1] != '.') return false;
    player = p[0] - '1';
    rest = p + 2;
    return true;
}

int buttonBit(const std::string& id)
{
    for (int i = 0; i < controls::kButtons; ++i)
        if (id == controls::kButtonKeys[i]) return i;
    return -1;
}

// The controller a player's controller page edits: theirs, or "" (the shared fallback).
std::string padGuid(int player, std::string* name = nullptr)
{
    const int index = gbarecomp::player_controller(player);
    const auto pads = gbarecomp::connected_controllers();
    if (index < 0 || index >= static_cast<int>(pads.size())) return std::string();
    if (name) *name = pads[index].name;
    return pads[index].guid;
}

// What the player reads on their controller ("Cross", "LB", "ZR"); generic names on the fallback page.
std::string padName(int player, const controls::PadBind& bind)
{
    const int index = gbarecomp::player_controller(player);
    const auto pads = gbarecomp::connected_controllers();
    const int type = index >= 0 && index < static_cast<int>(pads.size()) ? pads[index].type : 0;
    return controls::display_name(bind, controls::pad_style(type));
}

// Refreshes a player's device choices from what is connected now.
void refreshDevices(int player)
{
    PlayerRows& rows = g_rows[player];
    const controls::Settings settings = gbarecomp::player_controls();
    const auto pads = gbarecomp::connected_controllers();
    std::snprintf(rows.choiceText[kDeviceAuto], sizeof(rows.choiceText[0]), "Auto (keyboard + any controller)");
    std::snprintf(rows.choiceText[kDeviceKeyboard], sizeof(rows.choiceText[0]), "Keyboard");
    std::snprintf(rows.choiceText[kDeviceNone], sizeof(rows.choiceText[0]), "None");
    for (int i = 0; i < kPadSlots; ++i) {
        char* text = rows.choiceText[kFirstPad + i];
        if (i < static_cast<int>(pads.size())) {
            const int other = pads[i].player >= 0 && pads[i].player != player ? pads[i].player : -1;
            if (other >= 0)
                std::snprintf(text, sizeof(rows.choiceText[0]), "%s (player %d's)", pads[i].name.c_str(), other + 1);
            else
                std::snprintf(text, sizeof(rows.choiceText[0]), "%s", pads[i].name.c_str());
        } else {
            std::snprintf(text, sizeof(rows.choiceText[0]), "(no controller %d)", i + 1);
        }
    }
    // A chosen controller that is not connected shows in the first free slot.
    const controls::PlayerSetting& me = settings.player[player];
    if (me.source == controls::Source::Gamepad && !me.guid.empty() && gbarecomp::player_waiting_for_controller(player)) {
        const int slot = static_cast<int>(pads.size());
        if (slot < kPadSlots) {
            const auto found = settings.pad_names.find(me.guid);
            std::snprintf(rows.choiceText[kFirstPad + slot], sizeof(rows.choiceText[0]), "%s (not connected)",
                          found != settings.pad_names.end() ? found->second.c_str() : "Controller");
        }
    }
}

int deviceValue(int player)
{
    const controls::Settings settings = gbarecomp::player_controls();
    const controls::PlayerSetting& me = settings.player[player];
    switch (me.source) {
    case controls::Source::Auto: return kDeviceAuto;
    case controls::Source::Keyboard: return kDeviceKeyboard;
    case controls::Source::None: return kDeviceNone;
    case controls::Source::Gamepad: break;
    }
    const auto pads = gbarecomp::connected_controllers();
    for (int i = 0; i < static_cast<int>(pads.size()) && i < kPadSlots; ++i)
        if (pads[i].guid == me.guid || (me.guid.empty() && pads[i].player == player)) return kFirstPad + i;
    return kFirstPad + std::min(static_cast<int>(pads.size()), kPadSlots - 1);
}

void setDevice(int player, int value)
{
    controls::Settings settings = gbarecomp::player_controls();
    const auto pads = gbarecomp::connected_controllers();
    const int count = std::min(static_cast<int>(pads.size()), kPadSlots);
    // Stepping onto an empty controller slot moves on: forward to None, back to the last controller.
    if (value >= kFirstPad + count && value < kDeviceNone) {
        const int current = deviceValue(player);
        value = value > current ? kDeviceNone : (count ? kFirstPad + count - 1 : kDeviceKeyboard);
    }
    if (value == kDeviceAuto) {
        settings.player[player].source = controls::Source::Auto;
        settings.player[player].guid.clear();
    } else if (value == kDeviceKeyboard) {
        controls::claim_keyboard(settings, player);
    } else if (value == kDeviceNone) {
        settings.player[player].source = controls::Source::None;
        settings.player[player].guid.clear();
    } else {
        controls::claim_pad(settings, player, pads[value - kFirstPad].guid);
    }
    gbarecomp::set_player_controls(settings);
}

int menuGet(const char* key, int* out)
{
    int player = 0;
    std::string rest;
    if (!parseKey(key, player, rest)) return 0;
    if (rest == "device") {
        refreshDevices(player);
        *out = deviceValue(player);
        return 1;
    }
    if (rest == "deadzone") {
        *out = controls::profile_for(gbarecomp::player_controls(), padGuid(player)).deadzone_pct;
        return 1;
    }
    return 0;
}

int menuSet(const char* key, int value)
{
    int player = 0;
    std::string rest;
    if (!parseKey(key, player, rest)) return 0;
    if (rest == "device") {
        setDevice(player, value);
        return 1;
    }
    if (rest == "deadzone") {
        controls::Settings settings = gbarecomp::player_controls();
        const std::string guid = padGuid(player);
        controls::PadProfile& profile = guid.empty() ? settings.global : settings.pads[guid];
        profile.deadzone_pct = value;
        profile.deadzone_set = true;
        gbarecomp::set_player_controls(settings);
        return 1;
    }
    return 0;
}

int menuAction(const char* key)
{
    if (!std::strcmp(key, "rotk.controls.swap")) {
        controls::Settings settings = gbarecomp::player_controls();
        controls::swap_players(settings);
        gbarecomp::set_player_controls(settings);
        return 1;
    }
    int player = 0;
    std::string rest;
    if (!parseKey(key, player, rest)) return 0;
    // Capture starts return 0: the engine's prompt stays in the status line instead of "Done".
    if (rest == "claim") {
        gbarecomp::begin_device_claim(player);
        return 0;
    }
    if (rest.rfind("key.", 0) == 0 || rest.rfind("pad.", 0) == 0) {
        const int bit = buttonBit(rest.substr(4));
        if (bit < 0) return 0;
        gbarecomp::begin_binding_capture(player, bit, rest[0] == 'p');
        return 0;
    }
    if (rest == "reset-keys") {
        controls::Settings settings = gbarecomp::player_controls();
        controls::reset_keys(settings, player, gbarecomp::player_control_names());
        gbarecomp::set_player_controls(settings);
        return 1;
    }
    if (rest == "reset-pad") {
        controls::Settings settings = gbarecomp::player_controls();
        controls::reset_pad(settings, padGuid(player), gbarecomp::player_control_names());
        gbarecomp::set_player_controls(settings);
        return 1;
    }
    return 0;
}

int menuText(const char* key, char* buf, std::size_t size)
{
    int player = 0;
    std::string rest;
    if (!parseKey(key, player, rest)) return 0;
    const bool pad = rest.rfind("pad.", 0) == 0;
    if (!pad && rest.rfind("key.", 0) != 0) return 0;
    const int bit = buttonBit(rest.substr(4));
    if (bit < 0) return 0;
    const controls::Settings settings = gbarecomp::player_controls();
    if (pad) {
        std::string name;
        const std::string guid = padGuid(player, &name);
        // The page's first row says which controller it edits.
        std::snprintf(g_rows[player].padPageNote, sizeof(g_rows[player].padPageNote),
                      guid.empty() ? "Editing the layout every controller without its own uses."
                                   : "Editing %s's layout.",
                      name.c_str());
        std::snprintf(buf, size, "%s", padName(player, controls::profile_for(settings, guid).bind[bit]).c_str());
    } else {
        const int code = settings.player[player].keys[bit];
        const char* name = code > 0 ? gbarecomp::player_control_names().scancode_name(code) : nullptr;
        std::snprintf(buf, size, "%s", name && name[0] ? name : "(none)");
    }
    return 1;
}

RecompRuntimeUiItem item(const char* key, const char* section, const char* label, const char* description,
                         RecompRuntimeUiItemType type)
{
    RecompRuntimeUiItem it{};
    it.key = key;
    it.section = section;
    it.label = label;
    it.description = description;
    it.type = type;
    it.step = 1;
    return it;
}

}  // namespace

void controlsMenuRegister(gbarecomp::RunOptions& options)
{
    g_strings.reserve(256);   // keep() hands out c_str() pointers: no reallocation
    for (int p = 0; p < 2; ++p) {
        for (int i = 0; i < kDeviceCount; ++i) g_rows[p].choices[i] = g_rows[p].choiceText[i];
        refreshDevices(p);
    }
    const char* top = "Controls";
    for (int p = 0; p < 2; ++p) {
        const std::string n = std::to_string(p + 1);
        const std::string base = "rotk.controls.p" + n + ".";
        RecompRuntimeUiItem device = item(keep(base + "device"), top, keep("Player " + n + " device"),
                                          "Auto plays on the keyboard and the first free controller.",
                                          RECOMP_RUNTIME_UI_CHOICE);
        device.choices = g_rows[p].choices;
        device.choice_count = kDeviceCount;
        device.maximum = kDeviceCount - 1;
        g_items.push_back(device);
        g_items.push_back(item(keep(base + "claim"), top, keep("Player " + n + ": press to choose device"),
                               "Then press any key or controller button: that device becomes theirs.",
                               RECOMP_RUNTIME_UI_ACTION));
    }
    g_items.push_back(item("rotk.controls.swap", top, "Swap players",
                           "Player 1 and player 2 trade devices and keyboard keys.", RECOMP_RUNTIME_UI_ACTION));

    for (int p = 0; p < 2; ++p) {
        const std::string n = std::to_string(p + 1);
        const std::string base = "rotk.controls.p" + n + ".";
        const char* keys = keep("Controls/Player " + n + " keyboard");
        const char* pads = keep("Controls/Player " + n + " controller");
        for (int i = 0; i < controls::kButtons; ++i) {
            const int bit = kOrder[i];
            const char* label = controls::kButtonLabels[bit];
            g_items.push_back(item(keep(base + "key." + controls::kButtonKeys[bit]), keys, label,
                                   i == 0 ? "Select a button, then press the key for it. Esc cancels." : nullptr,
                                   RECOMP_RUNTIME_UI_BINDING));
        }
        g_items.push_back(item(keep(base + "reset-keys"), keys, "Reset keys",
                               p ? "Back to WASD, J/K, G, Space and U/I." : "Back to the arrows, X/Z, Return, Right Shift and C/V.",
                               RECOMP_RUNTIME_UI_ACTION));
        for (int i = 0; i < controls::kButtons; ++i) {
            const int bit = kOrder[i];
            g_items.push_back(item(keep(base + "pad." + controls::kButtonKeys[bit]), pads,
                                   controls::kButtonLabels[bit], i == 0 ? g_rows[p].padPageNote : nullptr,
                                   RECOMP_RUNTIME_UI_BINDING));
        }
        RecompRuntimeUiItem deadzone = item(keep(base + "deadzone"), pads, "Stick dead zone (%)",
                                            "How far the stick moves before it counts. The left stick always moves.",
                                            RECOMP_RUNTIME_UI_INT);
        deadzone.minimum = 5;
        deadzone.maximum = 95;
        deadzone.step = 5;
        g_items.push_back(deadzone);
        g_items.push_back(item(keep(base + "reset-pad"), pads, "Reset controller",
                               "Back to the standard layout for this controller.", RECOMP_RUNTIME_UI_ACTION));
    }
    options.ui_extra_items = g_items.data();
    options.ui_extra_item_count = g_items.size();
    options.ui_get = menuGet;
    options.ui_set = menuSet;
    options.ui_action = menuAction;
    options.ui_get_text = menuText;
}

void controlsApplyOldPlayerTwo()
{
    const char* keys[controls::kButtons];
    const char* pads[controls::kButtons];
    for (int i = 0; i < controls::kButtons; ++i) {
        keys[i] = oldValue(kOldButtons[i], false);
        pads[i] = oldValue(kOldButtons[i], true);
    }
    gbarecomp::set_player_two_controls(keys, pads);
}

}  // namespace rotk
