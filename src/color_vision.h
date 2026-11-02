#pragma once

#include "runtime.h"

namespace rotk {

// Color vision correction: the "color-vision" feature of the rotk.pc package, applied to each shown
// frame. Presentation only.
void colorVisionRegister();
bool colorVisionActive();
// The mode's name as ROTK_PC_COLOR_VISION takes it ("deutan", "protan", "tritan"), or nullptr when off.
const char* colorVisionModeName();
void colorVisionDecorate(const gbarecomp::PresentFrame* frame);

}  // namespace rotk
