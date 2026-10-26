#pragma once

#include <cstdint>

namespace rotk {

// Real transparency: the "transparency" feature of the rotk.pc package. The game fakes see-through
// sprites (water and ice reflections) by hiding them on every other vblank. This draws them on every
// frame with the GBA's semi-transparent sprite mode at the "strength" option (35% by default).
void transparencyRegister();

// Resolves the setting. Called from applyPcSettings.
void transparencyApply();

bool transparencyActive();

// The shadow buffer (0/1) and page (0/1) the VBlank handler last copied to OAM.
void transparencySetDisplayed(int buffer, int page);

// Presentation hook part: replaces flickering pieces in the line's OAM copy and sets the blend.
void transparencyScanline(std::uint32_t y, std::uint8_t* io, std::uint8_t* oam);

// Same for extra pieces (past 128): `pieces` is the displayed page, `page0`/`page1` both pages.
const std::uint8_t* transparencyAdjustExtra(const std::uint8_t* pieces, const std::uint8_t* page0,
                                            const std::uint8_t* page1, std::uint32_t count);

}  // namespace rotk
