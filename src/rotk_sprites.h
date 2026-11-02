#pragma once

namespace rotk {

// ROTK hero sprites in The Two Towers (plain host, docs/SUPER_GAME.md "Phase 5"): the heroes' poses
// drawn with their ROTK animations (ROTK_TTT_ROTK_SPRITES=1), and the dialog portraits with ROTK's
// (ROTK_TTT_ROTK_PORTRAITS=1), each optional; the ROTK ROM in ROTK_TTT_ROTK_ROM. Presentation and hit boxes only: the Two Towers' game code is untouched.
void rotkSpritesRegister();

}  // namespace rotk
