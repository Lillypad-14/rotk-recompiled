#pragma once

#include "runtime.h"

namespace rotk {

// Controller rumble: the "rumble" feature of the rotk.pc package. rumbleObserve runs at each shown
// frame and rumbles when the console's own player is hurt or levels up.
void rumbleRegister();
bool rumbleActive();
void rumbleObserve(const gbarecomp::PresentFrame* frame);

}  // namespace rotk
