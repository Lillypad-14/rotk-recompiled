#pragma once

#include <cstdint>
#include <vector>

namespace rotk {

// Instrument sample packs for the HD audio mixer (docs/AUDIO.md, "Sample packs"): a replacement holds
// the same sound as a ROM sample at `ratio` times as many frames, centred in s8 units (-128..127).
struct SampleReplacement {
    std::vector<float> frames;
    double ratio = 1.0;
};

// The ROM sample a voice plays: its bytes [start, end), looping like the voice.
struct SampleSpan {
    std::uint32_t start = 0, end = 0;
    std::uint8_t loopMode = 0;  // 0 one-shot, 1 forward loop, 2 ping-pong
    std::uint32_t loopLen = 0;
};

// Loads every pack in mods/packages/rotk.pc/<version>/samples/<pack>/ beside the executable.
void samplePacksLoad();
void samplePacksClear();
// The replacement for the sample starting at `start` whose length matches, or null.
const SampleReplacement* samplePackFind(const SampleSpan& span);
// ROTK_PC_SAMPLE_DUMP=<folder>: writes each ROM sample the mixer plays once, as <start>.wav, and
// lists it in samples.txt, so a pack can be made from them. `readByte` reads guest memory.
void samplePackNote(const SampleSpan& span, std::uint8_t (*readByte)(std::uint32_t));

}  // namespace rotk
