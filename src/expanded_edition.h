#pragma once

#include <string>
#include <vector>

#include "runtime.h"

namespace rotk {

// The launcher's "Expanded" row, in builds made with ROTK_EDITION_PATCH (the vanilla game): the
// Expanded mod is applied to the player's ROM as a BPS patch and the Expanded build plays it.
void expandedEditionRegister(gbarecomp::RunOptions& options);
// After the launcher: plays the Expanded build when the launcher chose it (making or reusing the
// Expanded ROM) and returns its exit code, or explains a ROM that can't be played this way and
// returns 1. -1 = boot the game as usual.
int expandedEditionDispatch(const std::vector<std::string>& args);

}  // namespace rotk
