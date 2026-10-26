#include "hd_audio.h"
#include "audio_envelope.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gba_bus.h"
#include "mod_audio.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace rotk {
namespace {

// ROTK's sound library mixes in software (docs/AUDIO.md has the full map):
//
//   sfx_libVBlank -> 0x03001080 takes 276-sample chunks of a 4-chunk ring (L at 0x02000000, R at
//   0x02000450, FIFO A left / B right, TM0 reload 0xFC00 = 16,384 Hz) and calls
//   mixFrame(outL, outR, count) = 0x03000C48, splitting chunks at the music tick.
//
// mixFrame walks 32 voices of 44 bytes at 0x020008B4. Each playing voice is mixed in spans by one of
// the routines in the table at 0x08280D10 (index = selector + interpolation flag), and the span ends
// are where the voice loops, bounces or stops. The host port below advances the voice state exactly
// like the guest (same integer maths, same span rounding) and renders every span in float.

constexpr std::uint32_t kMixFrame = 0x03000C48u;
constexpr std::uint32_t kVoices = 0x020008B4u;
constexpr std::uint32_t kVoiceSize = 44u;
constexpr std::uint32_t kVoiceCount = 32u;
constexpr std::uint32_t kMuteSite = 0x03000CC8u;      // beq to the silent path; the GBA mod's Mute makes it b
constexpr std::uint32_t kMuteBranch = 0xEA000091u;
constexpr std::uint32_t kGuestRate = 16384u;
constexpr std::uint32_t kOversample = 4u;             // 65,536 Hz, the mod audio producer rate
constexpr std::uint32_t kMaxCount = 2048u;            // guest samples per call; the game uses <= 276
constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kHookId = "rotk.pc-hd-audio";
constexpr const char* kPluginId = "rotk.pc.hd-audio";

// Voice fields.
enum : std::uint32_t {
    kVol = 0,         // u8 base volume (0 = silent, position still advances)
    kState = 2,       // u8 1 = playing
    kLoopMode = 3,    // u8 0 one-shot, 1 forward loop, 2 ping-pong
    kStart = 4,       // u8* first sample
    kPtr = 8,         // u8* current sample
    kEnd = 12,        // u8* end of the sample
    kLoopLen = 16,    // u32 loop length in bytes
    kStep = 20,       // s32 16.16 step, negative while a ping-pong voice runs backwards
    kHandle = 24,     // u32 index | serial << 8; 0 once released
    kFrac = 28,       // u16 position fraction
    kVolL = 30,       // u8
    kVolR = 31,       // u8
    kInterp = 32,     // u8 0 or 8: second routine table (linear interpolation)
    kSelector = 33,   // u8 routine: 0/2 stereo, 1 left, 3 right, 4 centre (left volume), 7 release
    kRamp = 36,       // u8 release ramp samples left (otherwise the volume category)
    kLoopStart = 40,  // u8* loop start; during the release ramp u16 level (12.4) ...
    kRampStep = 42,   // ... and u16 step per sample
};
constexpr std::uint8_t kSelectorRelease = 7u;

enum class Mode { Off, On, Verify };
enum class Interp { Guest, Linear, Cubic };

struct Voice {
    std::uint8_t b[kVoiceSize];
    std::uint8_t u8(std::uint32_t o) const { return b[o]; }
    std::uint16_t u16(std::uint32_t o) const { return static_cast<std::uint16_t>(b[o] | b[o + 1] << 8); }
    std::uint32_t u32(std::uint32_t o) const
    {
        return static_cast<std::uint32_t>(b[o]) | static_cast<std::uint32_t>(b[o + 1]) << 8 |
               static_cast<std::uint32_t>(b[o + 2]) << 16 | static_cast<std::uint32_t>(b[o + 3]) << 24;
    }
    std::int32_t s32(std::uint32_t o) const { return static_cast<std::int32_t>(u32(o)); }
    void set8(std::uint32_t o, std::uint32_t v) { b[o] = static_cast<std::uint8_t>(v); }
    void set16(std::uint32_t o, std::uint32_t v)
    {
        b[o] = static_cast<std::uint8_t>(v);
        b[o + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    void set32(std::uint32_t o, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
};

struct Stats {
    std::uint64_t calls = 0;
    std::uint64_t oddCounts = 0;      // count not a multiple of 4 (the guest drops the tail)
    std::uint64_t compared = 0;       // verify: voices compared
    std::uint64_t mismatches = 0;
    std::uint64_t driverStops = 0;    // verify: stopped by the driver between calls, not a mismatch
    std::uint64_t underflows = 0;
    std::uint64_t overflows = 0;
};

// ---- WAV dumps ------------------------------------------------------------------------------------

struct Wav {
    std::FILE* file = nullptr;
    std::uint32_t frames = 0;

    void open(const char* path, std::uint32_t rate)
    {
        file = std::fopen(path, "wb");
        if (!file) return;
        writeHeader(rate);
    }
    void writeHeader(std::uint32_t rate)
    {
        const std::uint32_t data = frames * 4u;
        const std::uint32_t header[] = {0x46464952u, 36u + data, 0x45564157u, 0x20746D66u, 16u,
                                        0x00020001u, rate, rate * 4u, 0x00100004u, 0x61746164u, data};
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(header, sizeof(header), 1, file);
        std::fseek(file, 0, SEEK_END);
    }
    void frame(float left, float right)
    {
        if (!file) return;
        const std::int16_t pcm[2] = {toPcm(left), toPcm(right)};
        std::fwrite(pcm, sizeof(pcm), 1, file);
        ++frames;
    }
    void close(std::uint32_t rate)
    {
        if (!file) return;
        writeHeader(rate);
        std::fclose(file);
        file = nullptr;
    }
    // 1.0 = full-scale FIFO sample (s8 128). Same scale as the guest dump.
    static std::int16_t toPcm(float v)
    {
        const float s = v * 32768.0f;
        return static_cast<std::int16_t>(s > 32767.0f ? 32767.0f : s < -32768.0f ? -32768.0f : s);
    }
};

// ---- State ----------------------------------------------------------------------------------------

Mode g_mode = Mode::Off;
Interp g_interp = Interp::Cubic;
Stats g_stats;
Voice g_predicted[kVoiceCount];
bool g_havePrediction = false;
std::uint32_t g_lastOutL = 0, g_lastOutR = 0, g_lastCount = 0;
GBAModAudioStream g_stream = GBA_MOD_AUDIO_STREAM_INVALID;
Wav g_hdWav, g_guestWav;

// Output of one mixFrame call at the oversampled rate, 1.0 = full-scale FIFO sample.
float g_mixL[kMaxCount * kOversample];
float g_mixR[kMaxCount * kOversample];

// Producer ring, filled by the hook and drained by the mod audio pull stream (both on the emulation
// thread). The guest mixes about two chunks ahead of the FIFO, so the ring holds about that much.
constexpr std::uint32_t kRingFrames = 1u << 14;
float g_ringL[kRingFrames], g_ringR[kRingFrames];
std::uint32_t g_ringRead = 0, g_ringFill = 0;
bool g_ringPrimed = false;
AudioEnvelope g_envelope;

const std::uint8_t* g_rom = nullptr;
std::size_t g_romSize = 0;

// ---- Sample fetch and rendering ---------------------------------------------------------------------

std::uint8_t readByte(std::uint32_t address)
{
    const std::uint32_t offset = address - 0x08000000u;
    if (g_rom && address >= 0x08000000u && offset < g_romSize) return g_rom[offset];
    return bus_read_u8(address);
}

// Centred sample at address `a` (signed 64-bit so neighbours before the start stay representable),
// folded into the voice's loop the way playback would reach it.
float sampleAt(const Voice& v, std::int64_t a)
{
    const std::int64_t start = v.u32(kStart), end = v.u32(kEnd);
    const std::int64_t loopLen = v.u32(kLoopLen), loopStart = v.u32(kLoopStart);
    if (a >= end) {
        if (v.u8(kLoopMode) == 0 || loopLen <= 0) return 0.0f;
        if (v.u8(kLoopMode) == 2) a = end - 1 - (a - end);
        else a = end - loopLen + (a - end) % loopLen;
    }
    if (v.u8(kLoopMode) == 2 && a < loopStart && v.s32(kStep) < 0) a = loopStart + (loopStart - a);
    if (a < start) a = start;
    if (a >= end) a = end - 1;
    return static_cast<float>(readByte(static_cast<std::uint32_t>(a))) - 128.0f;
}

void voiceGains(const Voice& v, float& gl, float& gr)
{
    constexpr float kScale = 1.0f / (128.0f * 128.0f);  // (sample - 128) * vol / 128, in s8 units
    const float l = v.u8(kVolL) * kScale, r = v.u8(kVolR) * kScale;
    switch (v.u8(kSelector)) {
    case 1: gl = l; gr = 0.0f; break;
    case 3: gl = 0.0f; gr = r; break;
    case 4: gl = l; gr = l; break;
    default: gl = l; gr = r; break;
    }
}

// Render guest samples [off, off + n) of the call for voice `v` as it stands before the span.
void renderSpan(const Voice& v, std::uint32_t off, std::uint32_t n)
{
    if (g_mode == Mode::Off || n == 0) return;
    const std::uint32_t k0 = off * kOversample, k1 = (off + n) * kOversample;
    float gl, gr;
    voiceGains(v, gl, gr);
    if (v.u8(kSelector) == kSelectorRelease) {
        // Linear ramp from the last sample level to the centre, 12.4 fixed point per guest sample.
        const float level = static_cast<std::int16_t>(v.u16(kLoopStart)) / 16.0f;
        const float step = static_cast<std::int16_t>(v.u16(kRampStep)) / 16.0f;
        const std::uint32_t ramp = v.u8(kRamp) < n ? v.u8(kRamp) : n;
        const std::uint32_t kr = (off + ramp) * kOversample;
        for (std::uint32_t k = k0; k < kr; ++k) {
            const float t = static_cast<float>(k - k0) / kOversample;
            const float s = level + t * step - 128.0f;
            g_mixL[k] += s * v.u8(kVolL) / (128.0f * 128.0f);
            g_mixR[k] += s * v.u8(kVolR) / (128.0f * 128.0f);
        }
        return;
    }
    const std::int64_t base = v.u32(kPtr);
    const double frac = v.u16(kFrac) / 65536.0;
    const double step = v.s32(kStep) / 65536.0;
    const bool linearGuest = v.u8(kInterp) != 0;
    for (std::uint32_t k = k0; k < k1; ++k) {
        const double t = static_cast<double>(k - k0) / kOversample;
        const double pos = frac + t * step;
        const double whole = std::floor(pos);
        const std::int64_t i = base + static_cast<std::int64_t>(whole);
        const float f = static_cast<float>(pos - whole);
        float s;
        switch (g_interp) {
        case Interp::Guest:
            s = sampleAt(v, i);
            if (linearGuest) s += (sampleAt(v, i + 1) - s) * f;
            break;
        case Interp::Linear: {
            const float a = sampleAt(v, i);
            s = a + (sampleAt(v, i + 1) - a) * f;
            break;
        }
        default: {
            // Catmull-Rom over the four neighbours.
            const float p0 = sampleAt(v, i - 1), p1 = sampleAt(v, i), p2 = sampleAt(v, i + 1),
                        p3 = sampleAt(v, i + 2);
            s = p1 + 0.5f * f *
                         (p2 - p0 + f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + f * (3.0f * (p1 - p2) + p3 - p0)));
            break;
        }
        }
        g_mixL[k] += s * gl;
        g_mixR[k] += s * gr;
    }
}

// ---- Guest-exact voice state port -------------------------------------------------------------------

// One routine call from the table: mixes `audible` samples at `off` and advances the voice by `n`
// (the guest passes the rounded span length, which can exceed what is left of the call).
void routine(Voice& v, std::uint32_t off, std::uint32_t n, std::uint32_t audible)
{
    renderSpan(v, off, audible);
    const std::uint32_t groups = n >> 2;
    if (v.u8(kSelector) == kSelectorRelease) {
        // 0x0804AF78 (ARM in ROM): ramp min(ramp, n) samples, 4 per pass.
        std::uint32_t ramp = v.u8(kRamp);
        std::uint32_t todo = ramp < n ? ramp : n;
        std::uint32_t level = v.u16(kLoopStart);
        const std::uint32_t step = v.u16(kRampStep);
        for (; todo >= 4; todo -= 4) {
            level = (level + 4u * step) & 0xFFFFu;
            ramp -= 4u;
        }
        v.set16(kLoopStart, level);
        v.set8(kRamp, ramp);
        if (v.u8(kRamp) == 0) v.set8(kState, 0);
        return;
    }
    const std::uint32_t pos = v.u16(kFrac) + static_cast<std::uint32_t>(v.s32(kStep)) * (groups * 4u);
    v.set32(kPtr, v.u32(kPtr) + static_cast<std::uint32_t>(static_cast<std::int32_t>(pos) >> 16));
    v.set16(kFrac, pos & 0xFFFFu);
}

// BIOS Div as used by mixFrame: skipped when the divisor is 0 (quotient = dividend, remainder 0).
void guestDiv(std::int32_t num, std::int32_t den, std::int32_t& q, std::int32_t& r)
{
    if (den == 0) {
        q = num;
        r = 0;
        return;
    }
    q = num / den;
    r = num % den;
}

void loopEnd(Voice& v)
{
    if (v.u8(kLoopMode) == 2) v.set32(kStep, static_cast<std::uint32_t>(-v.s32(kStep)));
    else v.set32(kPtr, v.u32(kPtr) - v.u32(kLoopLen));
}

// 0x03000F14: a voice with volume 0 only moves its position.
void silentAdvance(Voice& v, std::uint32_t count)
{
    const std::int32_t step = v.s32(kStep);
    const std::uint32_t adv = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                                  static_cast<std::uint32_t>(step >> 8) * count) >> 8) + 1u;
    const std::uint32_t end = v.u32(kEnd), loopLen = v.u32(kLoopLen), loopStart = v.u32(kLoopStart);
    std::uint32_t ptr = v.u32(kPtr) + adv;
    if (v.u8(kLoopMode) != 0 && loopLen == 0) return;  // the guest would spin forever
    switch (v.u8(kLoopMode)) {
    case 0:
        if (ptr >= end) v.set8(kState, 0);
        else v.set32(kPtr, ptr);
        return;
    case 2: {
        std::uint32_t bounces = 0;
        if (step >= 0) {
            while (ptr >= end) {
                ptr -= loopLen;
                ++bounces;
            }
            if (bounces & 1u) ptr = end - (ptr - loopStart);
        } else {
            while (ptr < loopStart) {
                ptr += loopLen;
                ++bounces;
            }
            if (bounces & 1u) ptr = loopStart + (end - ptr);
        }
        v.set32(kPtr, ptr);
        if (bounces & 1u) v.set32(kStep, static_cast<std::uint32_t>(-step));
        return;
    }
    default:
        while (ptr >= end) ptr -= loopLen;
        v.set32(kPtr, ptr);
        return;
    }
}

// 0x03000C48 for one voice.
void mixVoice(Voice& v, std::uint32_t count, bool muted)
{
    if (v.u8(kVol) == 0 || muted) {
        silentAdvance(v, count);
        return;
    }
    const std::uint8_t sel = static_cast<std::uint8_t>(v.u8(kSelector) + v.u8(kInterp));
    if ((sel & 7u) == 5u || (sel & 7u) == 6u) return;  // empty table slots; the guest would crash
    std::uint32_t off = 0;
    std::uint32_t n = count;
    for (;;) {
        const std::int32_t step = v.s32(kStep);
        const std::uint32_t ptr = v.u32(kPtr);
        const std::uint32_t frac = v.u16(kFrac);
        const std::int32_t adv = static_cast<std::int32_t>(static_cast<std::uint32_t>(step >> 8) * n) >> 8;
        std::int32_t num, den;
        if (step >= 0) {
            if (ptr + static_cast<std::uint32_t>(adv) + 1u < v.u32(kEnd)) {
                routine(v, off, n, n);
                return;
            }
            num = static_cast<std::int32_t>(((v.u32(kEnd) - ptr) << 14) - (frac >> 2));
            den = step >> 2;
        } else {
            if (ptr + static_cast<std::uint32_t>(adv) > v.u32(kLoopStart)) {
                routine(v, off, n, n);
                return;
            }
            num = static_cast<std::int32_t>((frac >> 2) + ((ptr - v.u32(kLoopStart)) << 14));
            den = static_cast<std::int32_t>(static_cast<std::uint32_t>(-step)) >> 2;
        }
        std::int32_t q, r;
        guestDiv(num, den, q, r);
        if (r != 0) ++q;
        if (q < -3) q = 0;  // a negative span would hang the guest; never seen
        if (q > static_cast<std::int32_t>(n)) {
            routine(v, off, n, n);
            return;
        }
        if (q & 3) q = (q & ~3) + 4;
        if (q >= static_cast<std::int32_t>(n)) {
            routine(v, off, static_cast<std::uint32_t>(q), n);
            if (v.u8(kLoopMode) == 0) v.set8(kState, 0);
            else loopEnd(v);
            return;
        }
        if (q != 0) {
            routine(v, off, static_cast<std::uint32_t>(q), static_cast<std::uint32_t>(q));
            off += static_cast<std::uint32_t>(q);
            n -= static_cast<std::uint32_t>(q);
        }
        if (v.u8(kLoopMode) == 0) {
            v.set8(kState, 0);
            return;
        }
        loopEnd(v);
        if (n == 0) return;
    }
}

void readVoices(Voice* voices)
{
    const std::uint8_t* ewram = gbarecomp::active_bus()->ewram_ptr();
    std::memcpy(voices, ewram + (kVoices & 0x3FFFFu), sizeof(Voice) * kVoiceCount);
}

void mixAll(Voice* voices, std::uint32_t count)
{
    const bool muted = bus_read_u32(kMuteSite) == kMuteBranch;
    static const bool debug = std::getenv("ROTK_PC_HD_AUDIO_DEBUG") != nullptr;
    if (debug && g_stats.calls % 600 == 0) {
        std::fprintf(stderr, "[rotk] hd audio call %llu: category volumes %u %u, shift word %08x\n",
                     static_cast<unsigned long long>(g_stats.calls), bus_read_u8(0x03000090u),
                     bus_read_u8(0x03000091u), bus_read_u32(0x03000B70u));
        for (std::uint32_t i = 0; i < kVoiceCount; ++i) {
            const Voice& v = voices[i];
            if (v.u8(kState) != 1) continue;
            std::fprintf(stderr, "  voice %2u vol %3u pan %4d L %3u R %3u sel %u interp %u cat %u mode %u step %d\n", i,
                         v.u8(kVol), static_cast<std::int8_t>(v.u8(1)), v.u8(kVolL), v.u8(kVolR), v.u8(kSelector),
                         v.u8(kInterp), v.u8(kRamp), v.u8(kLoopMode), v.s32(kStep));
        }
    }
    for (std::uint32_t i = 0; i < kVoiceCount; ++i)
        if (voices[i].u8(kState) == 1) mixVoice(voices[i], count, muted);
}

// ---- Verification (guest mixer still runs) ----------------------------------------------------------

void compareWithGuest()
{
    Voice actual[kVoiceCount];
    readVoices(actual);
    for (std::uint32_t i = 0; i < kVoiceCount; ++i) {
        const Voice& p = g_predicted[i];
        const Voice& a = actual[i];
        // The driver may start, stop or release a voice between two calls: compare only voices that
        // kept their handle and routine.
        if (p.u32(kHandle) != a.u32(kHandle) || p.u8(kSelector) != a.u8(kSelector)) continue;
        ++g_stats.compared;
        const bool same = p.u8(kState) == a.u8(kState) && p.u32(kPtr) == a.u32(kPtr) &&
                          p.u16(kFrac) == a.u16(kFrac) && (p.s32(kStep) < 0) == (a.s32(kStep) < 0) &&
                          p.u8(kRamp) == a.u8(kRamp);
        if (same) continue;
        // A voice the driver stopped in between (sfx stop, song change) keeps its handle and position.
        if (p.u8(kState) == 1 && a.u8(kState) == 0 && p.u32(kPtr) == a.u32(kPtr) && p.u16(kFrac) == a.u16(kFrac)) {
            ++g_stats.driverStops;
            continue;
        }
        if (++g_stats.mismatches <= 20)
            std::fprintf(stderr,
                         "[rotk] hd audio mismatch call %llu voice %u: state %u/%u ptr %08x/%08x frac %04x/%04x "
                         "step %d/%d ramp %u/%u mode %u sel %u\n",
                         static_cast<unsigned long long>(g_stats.calls), i, p.u8(kState), a.u8(kState),
                         p.u32(kPtr), a.u32(kPtr), p.u16(kFrac), a.u16(kFrac), p.s32(kStep), a.s32(kStep),
                         p.u8(kRamp), a.u8(kRamp), a.u8(kLoopMode), a.u8(kSelector));
    }
}

// The guest's own output of the previous call, still in its ring.
void dumpGuestOutput()
{
    if (!g_guestWav.file || g_lastCount == 0) return;
    const std::uint32_t written = g_lastCount & ~3u;
    for (std::uint32_t i = 0; i < written; ++i) {
        const auto l = static_cast<std::int8_t>(bus_read_u8(g_lastOutL + i));
        const auto r = static_cast<std::int8_t>(bus_read_u8(g_lastOutR + i));
        g_guestWav.frame(l / 128.0f, r / 128.0f);
    }
}

// ---- Output ------------------------------------------------------------------------------------------

void pushRing(std::uint32_t frames)
{
    for (std::uint32_t k = 0; k < frames; ++k) {
        if (g_ringFill == kRingFrames) {  // no consumer (fast-forward, muted run): drop the oldest
            g_ringRead = (g_ringRead + 1u) & (kRingFrames - 1u);
            --g_ringFill;
            ++g_stats.overflows;
        }
        const std::uint32_t w = (g_ringRead + g_ringFill) & (kRingFrames - 1u);
        g_ringL[w] = g_mixL[k];
        g_ringR[w] = g_mixR[k];
        ++g_ringFill;
    }
}

// Mod audio pull stream: interleaved stereo S16 at 65,536 Hz (the framework averages it on a mono
// device). Must always return max_frames, fewer ends the stream.
std::size_t pullStream(void*, std::int16_t* dst, std::size_t max_frames)
{
    // Same loudness as the native path: an s8 FIFO sample x plays as x * 192 (gba_audio.cpp).
    constexpr float kScale = 128.0f * 192.0f;
    auto pcm = [](float v) {
        const float s = v * kScale;
        return static_cast<std::int16_t>(s > 32767.0f ? 32767.0f : s < -32767.0f ? -32767.0f : s);
    };
    for (std::size_t i = 0; i < max_frames; ++i) {
        if (!g_ringPrimed && g_ringFill >= 2u * 276u * kOversample) g_ringPrimed = true;
        if (!g_ringPrimed || g_ringFill == 0) {
            if (g_ringPrimed) ++g_stats.underflows;
            g_ringPrimed = false;
            g_envelope.reset();
            dst[2 * i] = dst[2 * i + 1] = 0;
            continue;
        }
        const auto frame = g_envelope.apply(g_ringL[g_ringRead], g_ringR[g_ringRead], g_ringFill);
        dst[2 * i] = pcm(frame[0]);
        dst[2 * i + 1] = pcm(frame[1]);
        g_ringRead = (g_ringRead + 1u) & (kRingFrames - 1u);
        --g_ringFill;
    }
    return max_frames;
}

// ---- Hook --------------------------------------------------------------------------------------------

void returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    else cpu->cpsr &= ~0x20u;
}

int hdMixFrame(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t outL = cpu->R[0], outR = cpu->R[1], count = cpu->R[2];
    if (count == 0 || count > kMaxCount) return 0;
    // Replacing the mixer needs the host stream, which the activated package feature registers
    // (activation runs after hdAudioApply). Without it, only a WAV dump run may replace it.
    if (g_mode == Mode::On && g_stream == GBA_MOD_AUDIO_STREAM_INVALID && !g_hdWav.file) return 0;
    if (!g_rom) {
        g_rom = gbarecomp::active_bus()->rom_ptr();
        g_romSize = gbarecomp::active_bus()->rom_size();
    }
    ++g_stats.calls;
    if (count & 3u) ++g_stats.oddCounts;
    std::fill(g_mixL, g_mixL + count * kOversample, 0.0f);
    std::fill(g_mixR, g_mixR + count * kOversample, 0.0f);

    Voice voices[kVoiceCount];
    if (g_mode == Mode::Verify) {
        if (g_havePrediction) compareWithGuest();
        dumpGuestOutput();
        readVoices(voices);
        mixAll(voices, count);
        std::memcpy(g_predicted, voices, sizeof(voices));
        g_havePrediction = true;
        g_lastOutL = outL;
        g_lastOutR = outR;
        g_lastCount = count;
    } else {
        Voice before[kVoiceCount];
        readVoices(voices);
        std::memcpy(before, voices, sizeof(voices));
        mixAll(voices, count);
        for (std::uint32_t i = 0; i < kVoiceCount; ++i)
            for (std::uint32_t o = 0; o < kVoiceSize; ++o)
                if (voices[i].b[o] != before[i].b[o]) bus_write_u8(kVoices + i * kVoiceSize + o, voices[i].b[o]);
        // The FIFOs keep playing the guest ring: silence it, the host stream carries the sound.
        for (std::uint32_t i = 0; i < count; ++i) {
            bus_write_u8(outL + i, 0);
            bus_write_u8(outR + i, 0);
        }
        if (g_stream != GBA_MOD_AUDIO_STREAM_INVALID) gba_mod_audio_stream_play(g_stream, 100);
    }
    const std::uint32_t frames = count * kOversample;
    if (g_hdWav.file)
        for (std::uint32_t k = 0; k < frames; ++k) g_hdWav.frame(g_mixL[k], g_mixR[k]);
    if (g_stream != GBA_MOD_AUDIO_STREAM_INVALID) pushRing(frames);
    if (g_stats.calls == 1)
        std::fprintf(stderr, "[rotk] hd audio %s (%u Hz, %s)\n", g_mode == Mode::Verify ? "verifying" : "active",
                     kGuestRate * kOversample,
                     g_interp == Interp::Guest ? "guest interpolation" : g_interp == Interp::Linear ? "linear" : "cubic");
    if (g_mode == Mode::Verify) return 0;  // the guest mixer runs as well
    returnToCaller(cpu);
    return 1;
}

void report()
{
    g_hdWav.close(kGuestRate * kOversample);
    g_guestWav.close(kGuestRate);
    if (g_stats.calls == 0) return;
    std::fprintf(stderr,
                 "[rotk] hd audio: %llu calls, %llu odd counts, %llu voices compared, %llu mismatches "
                 "(%llu driver stops), %llu underflows, %llu dropped frames\n",
                 static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.oddCounts),
                 static_cast<unsigned long long>(g_stats.compared),
                 static_cast<unsigned long long>(g_stats.mismatches),
                 static_cast<unsigned long long>(g_stats.driverStops),
                 static_cast<unsigned long long>(g_stats.underflows),
                 static_cast<unsigned long long>(g_stats.overflows));
}

void hdAudioActivated()
{
    g_stream = gba_mod_audio_stream_register_s16_stereo(pullStream, nullptr);
    if (g_stream != GBA_MOD_AUDIO_STREAM_INVALID) gba_mod_audio_stream_set_enabled(g_stream, 1);
}

}  // namespace

void hdAudioRegister()
{
    gba_mod_register_function_entry_plugin(kHookId, kMixFrame, 0, hdMixFrame);
    gba_mod_register_activation_plugin(kPluginId, hdAudioActivated);
    std::atexit(report);
}

void hdAudioApply()
{
    // ROTK_PC_HD_AUDIO = 0 | 1 | verify, else the package feature (its option exists only when enabled).
    const char* interp = gba_mod_option_value(kPackageId, "hd-audio", "interpolation");
    g_mode = interp ? Mode::On : Mode::Off;
    if (const char* env = std::getenv("ROTK_PC_HD_AUDIO"))
        g_mode = std::string(env) == "verify" ? Mode::Verify : std::string(env) == "0" ? Mode::Off : Mode::On;
    if (const char* env = std::getenv("ROTK_PC_HD_AUDIO_INTERP")) interp = env;
    const std::string name = interp ? interp : "cubic";
    g_interp = name == "guest" ? Interp::Guest : name == "linear" ? Interp::Linear : Interp::Cubic;
    if (g_mode != Mode::Off) {
        if (const char* path = std::getenv("ROTK_PC_HD_AUDIO_WAV"))
            if (!g_hdWav.file) g_hdWav.open(path, kGuestRate * kOversample);
        if (const char* path = std::getenv("ROTK_PC_HD_AUDIO_GUEST_WAV"))
            if (g_mode == Mode::Verify && !g_guestWav.file) g_guestWav.open(path, kGuestRate);
    }
    g_havePrediction = false;
    g_ringRead = g_ringFill = 0;
    g_ringPrimed = false;
    g_envelope.reset();
    gba_mod_set_function_hook_enabled(kHookId, g_mode != Mode::Off);
}

}  // namespace rotk
