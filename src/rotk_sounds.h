#pragma once

namespace rotk {

// ROTK sound effects in The Two Towers (plain host): some Two Towers sound-effect ids play ROTK's
// sound instead (ROTK_TTT_ROTK_SOUNDS=1, the ROTK ROM in ROTK_TTT_ROTK_ROM). ROTK_TTT_TRACE_SFX=1
// logs every id the game plays. The game code is untouched: the Two Towers' sound engine (note
// sequences) differs from ROTK's (8-bit samples), so the ROTK sample is mixed in by the host.
void rotkSoundsRegister();

}  // namespace rotk
