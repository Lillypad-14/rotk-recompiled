#pragma once

namespace rotk {

// HD audio: the "hd-audio" feature of the rotk.pc package. Replaces the game's IWRAM software mixer
// with a host port that advances the same voice state and renders it in float at 4x the game's
// 16,384 Hz mix rate. See docs/AUDIO.md.
void hdAudioRegister();

// Resolves the setting (ROTK_PC_HD_AUDIO, else the package feature) and enables the hook. Called from
// applyPcSettings.
void hdAudioApply();

// Split-screen co-op: the port mixes for each console into its own guest ring at the game's rate (the
// session plays the consoles' FIFOs), with the game's interpolation when HD audio is off. Neither
// console then interprets the self-modifying mixer.
void hdAudioStartCoop();
void hdAudioEndCoop();

}  // namespace rotk
