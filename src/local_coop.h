#pragma once

#include "runtime.h"

namespace rotk {

// Split-screen co-op: the "local-coop" feature of the rotk.pc package. Choosing Multiplayer on the
// main menu opens a local link session of two fresh consoles (the framework's local link play),
// steers both into Multiplayer and shows them side by side. Back on the menu, the session ends and
// the solo game continues with console 1's save. See docs/SPLIT_SCREEN.md.
void localCoopRegister(gbarecomp::RunOptions& options);

// Provided by the PC feature set (src/main.cpp): per-console state for the presentation features
// and engine layout while the co-op consoles run. End restores the solo layout. The current
// console is 0 or 1, or -1 for the solo game.
void pcFeaturesStartCoop();
void pcFeaturesEndCoop();
void pcFeaturesSelectConsole(int console);

}  // namespace rotk
