#pragma once

#include "runtime.h"

namespace rotk {

// Split-screen co-op: the "local-coop" feature of the rotk.pc package. Choosing Multiplayer on the
// main menu opens a local link session of two fresh consoles (the framework's local link play),
// steers both into Multiplayer and shows them side by side. Back on the menu, the session ends and
// the solo game continues with console 1's save. See docs/SPLIT_SCREEN.md.
void localCoopRegister(gbarecomp::RunOptions& options);

}  // namespace rotk
