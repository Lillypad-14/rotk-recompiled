#pragma once

namespace rotk {

// ROTK's HUD in The Two Towers (plain host, docs/SUPER_GAME.md "ROTK HUD"): the bottom of the screen
// shows ROTK's HUD pieces, drawn the way ROTK draws them, filled from The Two Towers' health, spirit
// and experience. On with ROTK_TTT_ROTK_HUD=1 and the ROTK ROM in ROTK_TTT_ROTK_ROM.
void rotkHudRegister();

}  // namespace rotk
