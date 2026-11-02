#pragma once

#include <cstdint>

namespace rotk {

// Host palettes: sprites beyond the GBA's 16 OBJ palette banks. A piece whose actor's current
// animation carries a palette in host RAM (the converted Two Towers sprites) draws in those 16
// colors, whatever OBJ bank its OAM entry names, so such effects need none of the 7 shared banks.
// Presentation only. See docs/HOST_PALETTES.md.
void hostPalettesRegister();
// Enables it when the host RAM holds converted sprites. Called from applyPcSettings.
void hostPalettesApply(bool twoTowersData);
bool hostPalettesActive();
// Called by the sprite_pushOam hook for every piece: OAM index (128 and up for extra pieces) in
// shadow buffer 0 or 1.
void hostPalettesNotePiece(int buffer, std::uint32_t index);
// The buffer the VBlank handler last copied to OAM.
void hostPalettesSetDisplayed(int buffer);

}  // namespace rotk
