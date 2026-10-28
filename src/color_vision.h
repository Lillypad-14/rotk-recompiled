#pragma once

#include "runtime.h"

namespace rotk {

// Color vision correction: the "color-vision" feature of the rotk.pc package, applied to each shown
// frame. Presentation only.
void colorVisionRegister();
bool colorVisionActive();
void colorVisionDecorate(const gbarecomp::PresentFrame* frame);

}  // namespace rotk
