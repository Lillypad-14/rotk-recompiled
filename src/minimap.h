#pragma once

#include "runtime.h"

namespace rotk {

// Minimap: the "minimap" feature of the rotk.pc package, drawn into each shown frame by the host.
// Presentation only.
void minimapRegister();
bool minimapActive();
void minimapDecorate(const gbarecomp::PresentFrame* frame);

}  // namespace rotk
