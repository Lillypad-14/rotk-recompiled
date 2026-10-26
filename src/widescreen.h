#pragma once

#include <cstdint>

#include "runtime.h"

namespace rotk {

// Widescreen: the "widescreen" feature of the rotk.pc package. Registers its plugin and sets the
// view capabilities and per-frame policy on the run options.
void widescreenRegister(gbarecomp::RunOptions& options);

// Feed every completed DMA here. The game's per-frame copy of the visible world rows marks the
// camera the hardware tilemaps were built from.
void widescreenObserveDma(std::uint32_t source, std::uint32_t destination);

}  // namespace rotk
