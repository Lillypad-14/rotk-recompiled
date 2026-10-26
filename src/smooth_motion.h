#pragma once

#include <cstdint>

namespace rotk {

// Smooth motion: the "smooth-motion" feature of the rotk.pc package. The game ticks every other
// vblank; on the repeated vblank this draws the halfway point between the displayed tick and the
// next one, when the next one is already composed. Presentation only. See docs/SMOOTH_MOTION.md.
void smoothMotionRegister();

// Resolves the setting and enables the hooks. Called from applyPcSettings.
void smoothMotionApply();

// Installs the presentation scanline hook while smooth motion or real transparency is on.
void smoothRefreshPresentHook();

// True while the feature needs the sprite push/end-frame observers.
bool smoothMotionActive();

// Called by the sprite_pushOam hook for every piece: OAM index (128 and up for extra pieces) in
// shadow buffer 0 or 1.
void smoothNotePiece(int buffer, std::uint32_t index);

// Called by the sprite_endFrame hook with the buffer just composed.
void smoothEndFrame(int buffer);

// The buffer the VBlank handler last copied to OAM.
void smoothSetDisplayedBuffer(int buffer);

// Pixels the current presented frame adds to `bg`'s scroll on `axis` (0 = x, 1 = y): the halfway
// shift on an interpolated frame, otherwise 0. Valid while that frame's scanlines are drawn.
int smoothBgShift(int bg, int axis);

// Moves extra pieces (OAM format, `count` entries of 8 bytes) like hardware OAM on an interpolated
// frame. Returns the pointer to draw from.
const std::uint8_t* smoothAdjustExtra(const std::uint8_t* pieces, std::uint32_t count);

}  // namespace rotk
