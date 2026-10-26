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

// Split-screen co-op: whether the wide view is on (the co-op consoles then share it), fresh state
// for both co-op consoles with widescreen's hooks serving them, and the console whose state is in
// use (0 or 1, or -1 for the solo game).
bool widescreenEnabled();
void widescreenStartCoop();
void widescreenEndCoop();
void widescreenSelectConsole(int console);

}  // namespace rotk
