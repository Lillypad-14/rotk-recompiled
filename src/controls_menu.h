#pragma once

#include "runtime.h"

namespace rotk {

// The in-game menu's Controls section: each player's device (Auto, the keyboard, a controller,
// None), press to choose a device, swap players, and press-to-bind pages for each player's keys
// and controller. Everything lives in the engine's player controls (gbarecomp::player_controls),
// the same files the launcher's Controller page edits.
void controlsMenuRegister(gbarecomp::RunOptions& options);

// Hands the launcher's old player 2 options ("local-coop" feature) to the engine, which migrates
// them once (gbarecomp::set_player_two_controls). The local-coop feature's activation calls it.
void controlsApplyOldPlayerTwo();

}  // namespace rotk
