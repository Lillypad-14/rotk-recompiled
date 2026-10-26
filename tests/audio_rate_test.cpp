#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Synthetic PCM only. Check SDL's actual conversion for the four hardware
// rates, independently of the game's driver and the physical sound device.
int main() {
    int failures = 0;
    for (int rate : {32768, 65536, 131072, 262144}) {
        SDL_AudioStream* stream = SDL_NewAudioStream(AUDIO_S16SYS, 1, rate,
                                                    AUDIO_S16SYS, 1, 65536);
        if (!stream) return 2;
        std::vector<short> input(rate), output;
        for (int i = 0; i < rate; ++i)
            input[i] = static_cast<short>(12000 * std::sin(2 * 3.141592653589793 * 700 * i / rate));
        short block[4096];
        auto drain = [&]() {
            int bytes;
            while ((bytes = SDL_AudioStreamGet(stream, block, sizeof(block))) > 0)
                output.insert(output.end(), block, block + bytes / sizeof(short));
        };
        for (int i = 0; i < rate; i += 127) {
            if (SDL_AudioStreamPut(stream, input.data() + i,
                    std::min(127, rate - i) * sizeof(short)) < 0) return 2;
            drain();
        }
        SDL_AudioStreamFlush(stream);
        drain();
        SDL_FreeAudioStream(stream);
        int rising = 0;
        for (std::size_t i = 1; i < output.size(); ++i)
            if (output[i - 1] <= 0 && output[i] > 0) ++rising;
        // SDL2 retains a short resampler tail even after Flush on this SDK.
        // Allow at most 1,024 missing startup/tail frames (15.625 ms), but
        // measure pitch against the duration actually returned. A factor-two
        // rate mistake fails both the duration and the frequency contract.
        double expected_cycles = 700.0 * output.size() / 65536.0;
        bool ok = output.size() >= 64512 && output.size() <= 65536
                  && std::abs(rising - expected_cycles) <= 2;
        std::printf("%d Hz: %zu output samples, %d tone cycles: %s\n",
                    rate, output.size(), rising, ok ? "PASS" : "FAIL");
        failures += !ok;
    }
    return failures ? 1 : 0;
}
