#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime.h"

namespace rotk {

// The super game (docs/SUPER_GAME.md, phase 2): the mod's "The Two Towers" main menu row hands the
// window to The Two Towers (the plain-host build, TTT-Recompiled.exe) and takes it back when it
// quits. This game waits, paused and muted, in the meantime.

// The launcher's row for The Two Towers ROM. A valid pick becomes ROTK_PC_TWO_TOWERS_ROM, which
// twoTowersFind and the Expanded build it starts read first.
void superGameRegister(gbarecomp::RunOptions& options);
// Finds what The Two Towers needs (its executable, ROM and a BIOS) after twoTowersFind.
void superGameFind(const std::vector<std::string>& args);
// Whether the main menu can offer The Two Towers.
bool superGameAvailable();
// Once a frame: plays The Two Towers when the mod asked for it (PcHostInfo.superGame) and returns
// when it has quit. superGameField is the host RAM address of that field.
void superGamePoll(std::uint32_t superGameField);

}  // namespace rotk
