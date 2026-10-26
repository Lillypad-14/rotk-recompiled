#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace rotk {
// Presentation only: 1.95 ms at 65536 Hz. Leave a healthy stream at unity
// gain, but approach zero before starvation and ramp back in after re-prime.
struct AudioEnvelope {
    static constexpr std::uint32_t kFrames = 128;
    float gain = 0;
    void reset() { gain = 0; }
    std::array<float, 2> apply(float left, float right, std::uint32_t available) {
        const float tail = std::min(1.0f, static_cast<float>(available) / kFrames);
        gain = std::min(tail, gain + 1.0f / kFrames);
        return {left * gain, right * gain};
    }
};
}
