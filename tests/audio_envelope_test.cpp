#include <cmath>
#include <cstdio>
#include "audio_envelope.h"

int main() {
    rotk::AudioEnvelope envelope;
    float previous = 0;
    int failures = 0;
    auto step = [&](unsigned available) {
        auto out = envelope.apply(0.75f, -0.25f, available);
        if (std::abs(out[0] - previous) > 0.75f / 128 + 0.000001f) ++failures;
        if (std::abs(out[0] + out[1] * 3) > 0.000001f) ++failures;
        previous = out[0];
    };
    for (unsigned i = 0; i < 256; ++i) step(1000);
    if (previous != 0.75f) ++failures; // healthy stream stays at unity
    for (unsigned i = 128; i > 0; --i) step(i);
    step(0);
    if (previous != 0) ++failures;
    envelope.reset();
    for (unsigned i = 0; i < 128; ++i) step(1000);
    if (previous != 0.75f) ++failures;
    std::puts(failures ? "audio envelope FAILED" : "audio starvation/restart envelope passed");
    return failures ? 1 : 0;
}
