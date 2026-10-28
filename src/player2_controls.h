#pragma once

#include "runtime.h"

namespace rotk {

// Player 2's split-screen controls: launcher options of the "local-coop" feature and a "Player 2
// controls" section in the in-game menu (which saves them to the same options).
void player2ControlsRegister(gbarecomp::RunOptions& options);
// Applies the chosen controls; the local-coop feature's activation calls it.
void player2ControlsApply();

}  // namespace rotk
