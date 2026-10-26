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

// Called for every sprite piece the game pushes (OAM index, or 128 and up for extra pieces) into
// shadow OAM buffer 0 or 1, and with the buffer the VBlank handler last copied to OAM.
void widescreenNotePiece(int buffer, std::uint32_t index);
void widescreenSetDisplayedBuffer(int buffer);

}  // namespace rotk
