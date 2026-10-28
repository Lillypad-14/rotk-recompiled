#pragma once

#include "runtime.h"

namespace rotk {

// Enemy health bars and damage numbers: the "combat-info" feature of the rotk.pc package, drawn
// into each shown frame by the host. Presentation only.
void combatInfoRegister();
bool combatInfoActive();
void combatInfoDecorate(const gbarecomp::PresentFrame* frame);

}  // namespace rotk
