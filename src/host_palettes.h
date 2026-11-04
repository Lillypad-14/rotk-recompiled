#pragma once

#include <cstdint>

namespace rotk {

// Host palettes: sprites beyond the GBA's 16 OBJ palette banks. A piece whose actor's current
// animation carries a palette in host RAM (the converted Two Towers sprites) draws in those 16
// colors, whatever OBJ bank its OAM entry names, so such effects need none of the 7 shared banks.
// Presentation only. See docs/HOST_PALETTES.md.
void hostPalettesRegister();
// Piece palettes: a ROM frame set tagged "HPAL" (the romhack's hero anims with "#rrggbb" colors)
// gives each of its pieces up to 16 colors of its own, so one frame can show hundreds of colors.
// Called from applyPcSettings. twoTowersData also enables the converted sprites' host-RAM palettes.
void hostPalettesApply(bool twoTowersData);
bool hostPalettesActive();
// Converted Two Towers sprites draw in their own colors (PcHostInfo features bit 0).
bool hostPalettesConvertedSprites();
// Called by the sprite_pushOam hook for every piece: OAM index (128 and up for extra pieces) in
// shadow buffer 0 or 1, and the guest address of the 8-byte OAM entry being pushed.
void hostPalettesNotePiece(int buffer, std::uint32_t index, std::uint32_t oam);
// The buffer the VBlank handler last copied to OAM.
void hostPalettesSetDisplayed(int buffer);

}  // namespace rotk
