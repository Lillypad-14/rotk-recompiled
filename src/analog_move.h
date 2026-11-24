#pragma once

// Analog stick movement: the guest's player tick uses one fixed 16.16 speed
// per actor. The host publishes the left stick's magnitude per player and this
// hook scales the player's movement call; menus, NPCs and digital input keep
// their original speed. ROTK_PC_ANALOG_MOVE=0 disables it.
namespace rotk {

void analogMoveRegister();

}  // namespace rotk
