#include "player2_controls.h"

#include <cstring>
#include <string>
#include <vector>

#include "mod_runtime.h"
#include "recomp_runtime_ui.h"

namespace rotk {
namespace {

// Player 2's split-screen controls: a keyboard key and a controller button per GBA button, set in
// the launcher (options of the "local-coop" feature, mods/rotk.pc.manifest.toml.in) or in the
// in-game menu's "Player 2 controls" section, which saves them the same way. The values are SDL key
// and controller button names, applied with gbarecomp::set_player_two_controls.

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "local-coop";

// GBA buttons in KEYINPUT bit order, with their option id suffix and default controls.
struct Button {
    const char* id;
    const char* label;
    const char* key;
    const char* pad;
};
constexpr Button kButtons[10] = {
    {"a", "A", "J", "a"},
    {"b", "B", "K", "b"},
    {"select", "Select", "G", "back"},
    {"start", "Start", "Space", "start"},
    {"right", "Right", "D", "dpright"},
    {"left", "Left", "A", "dpleft"},
    {"up", "Up", "W", "dpup"},
    {"down", "Down", "S", "dpdown"},
    {"r", "R", "I", "rightshoulder"},
    {"l", "L", "U", "leftshoulder"},
};

// The choices, as SDL names. The manifest's choices for the same options are this list.
constexpr const char* kKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Space", "Return", "Tab", "Backspace", "Left Shift", "Right Shift", "Left Ctrl", "Right Ctrl",
    "Left Alt", "Right Alt", "Up", "Down", "Left", "Right", ";", "'", ",", ".", "/", "[", "]",
    "Keypad 0", "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4", "Keypad 5", "Keypad 6", "Keypad 7",
    "Keypad 8", "Keypad 9", "Keypad Enter", "Keypad +", "Keypad -",
};
constexpr const char* kPads[] = {
    "a", "b", "x", "y", "back", "start", "leftshoulder", "rightshoulder", "leftstick", "rightstick",
    "dpup", "dpdown", "dpleft", "dpright",
};
constexpr const char* kPadLabels[] = {
    "A", "B", "X", "Y", "Back", "Start", "Left bumper", "Right bumper", "Left stick press",
    "Right stick press", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
};
constexpr int kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
constexpr int kPadCount = sizeof(kPads) / sizeof(kPads[0]);

std::string optionId(const Button& button, bool pad)
{
    return std::string(pad ? "p2-pad-" : "p2-key-") + button.id;
}

const char* value(const Button& button, bool pad)
{
    const char* chosen = gba_mod_option_value(kPackageId, kFeatureId, optionId(button, pad).c_str());
    return chosen ? chosen : pad ? button.pad : button.key;
}

void apply()
{
    const char* keys[10];
    const char* pads[10];
    for (int i = 0; i < 10; ++i) {
        keys[i] = value(kButtons[i], false);
        pads[i] = value(kButtons[i], true);
    }
    gbarecomp::set_player_two_controls(keys, pads);
}

// In-game menu rows: keys "rotk.p2-key-<id>" and "rotk.p2-pad-<id>".
std::vector<std::string> g_keys, g_labels;
std::vector<RecompRuntimeUiItem> g_items;

const Button* rowButton(const char* key, bool& pad)
{
    for (const Button& button : kButtons)
        for (int p = 0; p < 2; ++p)
            if (("rotk." + optionId(button, p != 0)) == key) {
                pad = p != 0;
                return &button;
            }
    return nullptr;
}

int menuGet(const char* key, int* out)
{
    bool pad = false;
    const Button* button = rowButton(key, pad);
    if (!button) return 0;
    const char* current = value(*button, pad);
    const char* const* list = pad ? kPads : kKeys;
    const int count = pad ? kPadCount : kKeyCount;
    *out = 0;
    for (int i = 0; i < count; ++i)
        if (!std::strcmp(list[i], current)) *out = i;
    return 1;
}

int menuSet(const char* key, int index)
{
    bool pad = false;
    const Button* button = rowButton(key, pad);
    if (!button) return 0;
    const int count = pad ? kPadCount : kKeyCount;
    if (index < 0 || index >= count) return 1;
    gba_mod_set_option_value(kPackageId, kFeatureId, optionId(*button, pad).c_str(),
                             pad ? kPads[index] : kKeys[index]);
    apply();
    return 1;
}

}  // namespace

void player2ControlsRegister(gbarecomp::RunOptions& options)
{
    for (const Button& button : kButtons)
        for (int pad = 0; pad < 2; ++pad) {
            g_keys.push_back("rotk." + optionId(button, pad != 0));
            g_labels.push_back(std::string(button.label) + (pad ? " (controller)" : " (keyboard)"));
        }
    for (std::size_t i = 0; i < g_keys.size(); ++i) {
        const bool pad = i % 2 != 0;
        RecompRuntimeUiItem item{};
        item.key = g_keys[i].c_str();
        item.section = "Player 2 controls";
        item.label = g_labels[i].c_str();
        item.description = pad ? "Player 2's controller button for this GBA button (the left stick also moves)."
                               : "Player 2's keyboard key for this GBA button.";
        item.type = RECOMP_RUNTIME_UI_CHOICE;
        item.choices = pad ? kPadLabels : kKeys;
        item.choice_count = pad ? kPadCount : kKeyCount;
        item.minimum = 0;
        item.maximum = static_cast<int>(item.choice_count) - 1;
        item.step = 1;
        g_items.push_back(item);
    }
    options.ui_extra_items = g_items.data();
    options.ui_extra_item_count = g_items.size();
    options.ui_get = menuGet;
    options.ui_set = menuSet;
}

void player2ControlsApply()
{
    apply();
}

}  // namespace rotk
