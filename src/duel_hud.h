#pragma once

#include "runtime.h"

namespace rotk {

// The Expanded mod's Versus duel scoreboard, drawn at the top of each split-screen view while the
// mod reports a duel (its PcHostInfo.duel word). Presentation only.
void duelHudSetCoop(bool active);
bool duelHudActive();
void duelHudDecorate(const gbarecomp::PresentFrame* frame);

}  // namespace rotk
