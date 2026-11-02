#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime.h"

namespace rotk {

// The super game (docs/SUPER_GAME.md, phase 2): The Two Towers (the plain-host build,
// TTT-Recompiled.exe) starts hidden beside this game and waits at its main menu; the mod's "The Two
// Towers" main menu row fades out and hands it the window, its "Return of the King" row hands it
// back. This game waits, paused and muted, in the meantime.

// The launcher's row for The Two Towers ROM. A valid pick becomes ROTK_PC_TWO_TOWERS_ROM, which
// twoTowersFind and the Expanded build it starts read first.
void superGameRegister(gbarecomp::RunOptions& options);
// Finds what The Two Towers needs (its executable, ROM and a BIOS) after twoTowersFind.
void superGameFind(const std::vector<std::string>& args);
// Whether the main menu can offer The Two Towers.
bool superGameAvailable();
// Once a frame: keeps The Two Towers ready, and plays it (after a fade) when the mod asked for it
// (PcHostInfo.superGame), returning when it hands the window back. superGameField is the host RAM
// address of that field.
void superGamePoll(std::uint32_t superGameField);
// The fades around a switch, on this game's frames.
void superGameDecorate(const gbarecomp::PresentFrame* frame);
bool superGameDecorateActive();

}  // namespace rotk
