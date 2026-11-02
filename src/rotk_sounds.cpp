#include "rotk_sounds.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "gba_bus.h"
#include "mod_audio.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace rotk {
namespace {

using Bytes = std::vector<std::uint8_t>;

// The Two Towers: sfx_play(id) at 0x0803DCA0 plays when bit 2 of the sound settings byte +0x0C is
// set, at the volume in +0x0D (struct at the word 0x0803DCB4 points to).
constexpr std::uint32_t kTttSfxPlay = 0x0803DCA0u, kTttSoundSettingsPointer = 0x0803DCB4u;
constexpr const char* kHookId = "ttt.rotk-sounds-play";

// ROTK (stock ROM): sfx_play's tables. SfxEntries[id] = {kind 1 single / 2 variant set, index};
// SfxSingles[index] and SfxVariantSets[index][8] = {sound, volume}; SoundData[sound] -> a sample:
// +0 loop (0: one-shot), +4 end address, +8 rate (0x4000 = 16,384 Hz, the mix rate), +12 flags,
// unsigned 8-bit samples from +16.
constexpr std::uint32_t kRomBase = 0x08000000u;
constexpr std::uint32_t kSoundData = 0x082808D4u, kSfxSingles = 0x08287DF8u, kSfxVariantSets = 0x08288010u,
                        kSfxEntries = 0x08289450u;
constexpr std::uint32_t kRotkMixRate = 16384u;

// A Two Towers sound played from [from, to) (any caller when to is 0; any id when tt is kAnyId)
// plays a ROTK sound effect id instead.
constexpr std::uint32_t kAnyId = 0xFFFFFFFFu;
struct Swap {
    std::uint32_t tt, from, to, rotk;
    const char* what;
};
constexpr Swap kSwaps[] = {
    // loot (actor_lootPile 0x0802538C..0x08025B38): ids used only there
    {0x58u, 0, 0, 0x29u, "gem pickup"},       // after the gem add (player +0x0C, capped 0xFFFF) at 0x08025996
    {0x63u, 0, 0, 0x2Du, "item pickup"},      // items and artifacts
    {0x62u, 0, 0, 0x2Fu, "health pickup"},    // after player_addHp / player_addSpirit (half the maximum)
    {0x79u, 0x08025700u, 0x08025720u, 0x2Au, "pickup refused"},  // 0x79 is the menus' error sound too
    {0x87u, 0, 0, 0x10Au, "chest opens"},     // actor_chest_trigger
    {0x70u, 0, 0, 0x11Au, "level up"},        // player_awardXp
    // melee result (0x0800CD94): the ids come from the foe type's row (0x24 bytes), so by call site
    {kAnyId, 0x0800D04Cu, 0x0800D050u, 0x07u, "melee hit"},       // row +0x1A
    {kAnyId, 0x0800D010u, 0x0800D014u, 0x11u, "melee critical"},  // row +0x1C, damage doubled
    {kAnyId, 0x0800D1CCu, 0x0800D1D0u, 0x0Au, "melee miss"},      // row +0x20, the hit roll failed
    {0x57u, 0, 0, 0x13u, "missile hit"},      // fx_projectile_hit
};
// The heroes' own sounds: per hero 4 ids (8 bytes), the Two Towers' at 0x080502B8 (player_tick: entry
// 0 when an attack starts, 1 and 2 together, 3 at 0x0801CA6A) and ROTK's at 0x080585D4 (entry 0 on
// each combo swing), same hero order and layout. Played from player_tick.
constexpr std::uint32_t kTttHeroSounds[6][4] = {{0x3D, 0x46, 0x3F, 0x3E}, {0x65, 0x6F, 0x68, 0x65},
                                                {0x00, 0x0A, 0x03, 0x01}, {0x49, 0x53, 0x4B, 0x4A},
                                                {0x27, 0x30, 0x2B, 0x28}, {0x59, 0x60, 0x5E, 0x5A}};
constexpr std::uint32_t kRotkHeroSounds = 0x080585D4u;
constexpr std::uint32_t kPlayerTickFrom = 0x0801AFC8u, kPlayerTickTo = 0x0801CB00u;

struct State {
    bool swap = false, trace = false;
    // ROTK_TTT_TEST_SWAP=<hex id> (tests): that Two Towers id also plays the first swap's sound
    std::uint32_t testId = 0xFFFFFFFFu;
    Bytes rom;
    std::vector<Swap> swaps;
    std::vector<std::pair<std::uint32_t, GBAModAudioClip>> clips;  // by ROTK id
    std::uint32_t heroSwing[6] = {};                                // ROTK id per hero
} g_state;

std::uint32_t r8(std::uint32_t a) { return a - kRomBase < g_state.rom.size() ? g_state.rom[a - kRomBase] : 0u; }
std::uint32_t r16(std::uint32_t a) { return r8(a) | r8(a + 1) << 8; }
std::uint32_t r32(std::uint32_t a) { return r16(a) | r16(a + 2) << 16; }

// ROTK sound effect id's sample (the first sounding variant) as a 65,536 Hz clip.
GBAModAudioClip rotkClip(std::uint32_t id)
{
    const std::uint32_t kind = r16(kSfxEntries + id * 4), index = r16(kSfxEntries + id * 4 + 2);
    std::uint32_t sound = 0xFF;
    if (kind == 1) sound = r16(kSfxSingles + index * 4);
    else if (kind == 2)
        for (std::uint32_t v = 0; v < 8 && sound == 0xFF; ++v) sound = r16(kSfxVariantSets + index * 32 + v * 4);
    if (sound == 0xFF) return GBA_MOD_AUDIO_CLIP_INVALID;
    const std::uint32_t sample = r32(kSoundData + sound * 4);
    const std::uint32_t start = sample + 16, end = r32(sample + 4), rate = r32(sample + 8);
    if (end <= start || end - start > 0x100000u || !rate) return GBA_MOD_AUDIO_CLIP_INVALID;
    // source samples per output frame
    const double step = static_cast<double>(rate) / 0x4000 * kRotkMixRate / GBA_MOD_AUDIO_SAMPLE_RATE;
    const std::uint32_t count = end - start;
    std::vector<std::int16_t> pcm;
    for (double at = 0; at < count - 1; at += step) {
        const std::uint32_t i = static_cast<std::uint32_t>(at);
        const double f = at - i;
        const double a = static_cast<double>(r8(start + i)) - 128.0, b = static_cast<double>(r8(start + i + 1)) - 128.0;
        // ROTK mixes one voice at about (sample - 128) in its s8 output: half of full scale here
        pcm.push_back(static_cast<std::int16_t>((a + (b - a) * f) * 128.0));
    }
    std::fprintf(stderr, "[ttt] ROTK sound %02X: sample %u at %08X, %u bytes, rate %X -> %zu frames\n", id, sound,
                 sample, count, rate, pcm.size());
    // ROTK_TTT_SFX_WAV=<folder> (tests): each clip as rotk_<id>.wav
    if (const char* folder = std::getenv("ROTK_TTT_SFX_WAV")) {
        char name[512];
        std::snprintf(name, sizeof name, "%s/rotk_%02X.wav", folder, id);
        if (std::FILE* f = std::fopen(name, "wb")) {
            const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * 2);
            auto put32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto put16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); put32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
            put32(16); put16(1); put16(1); put32(GBA_MOD_AUDIO_SAMPLE_RATE); put32(GBA_MOD_AUDIO_SAMPLE_RATE * 2);
            put16(2); put16(16); std::fwrite("data", 1, 4, f); put32(bytes);
            std::fwrite(pcm.data(), 2, pcm.size(), f);
            std::fclose(f);
        }
    }
    return gba_mod_audio_register_pcm_s16_mono(pcm.data(), static_cast<std::uint32_t>(pcm.size()),
                                               GBA_MOD_AUDIO_SAMPLE_RATE);
}

int returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    return 1;
}

GBAModAudioClip clipFor(std::uint32_t rotkId)
{
    for (const auto& [id, clip] : g_state.clips)
        if (id == rotkId) return clip;
    return GBA_MOD_AUDIO_CLIP_INVALID;
}

// Plays ROTK sound effect rotkId at the Two Towers' sound-effect volume; false when sound is off.
bool playRotk(std::uint32_t rotkId, const char* what, std::uint32_t ttId)
{
    const std::uint32_t settings = bus_read_u32(kTttSoundSettingsPointer);
    if (!(bus_read_u8(settings + 0x0C) & 4u)) return false;
    const GBAModAudioClip clip = clipFor(rotkId);
    if (clip == GBA_MOD_AUDIO_CLIP_INVALID) return false;
    gba_mod_audio_set_enabled(clip, 1);
    const int gain = static_cast<int>(bus_read_u8(settings + 0x0D) * 100u / 255u);
    const int played = gba_mod_audio_play(clip, gain, 0);
    if (g_state.trace)
        std::fprintf(stderr, "[sfx] %02X -> ROTK %02X %s, gain %d, played %d\n", ttId, rotkId, what, gain, played);
    return true;
}

int sfxPlayEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t id = cpu->R[0], caller = (cpu->R[14] & ~1u) - 4;
    const std::uint32_t settings = bus_read_u32(kTttSoundSettingsPointer);
    const bool on = bus_read_u8(settings + 0x0C) & 4u;
    if (g_state.trace) std::fprintf(stderr, "[sfx] %02X from %08X%s\n", id, caller, on ? "" : " (sound off)");
    if (!g_state.swap || !on) return 0;
    for (std::size_t i = 0; i < g_state.swaps.size(); ++i) {
        const Swap& swap = g_state.swaps[i];
        const bool idMatch = swap.tt == kAnyId || swap.tt == id || (i == 0 && id == g_state.testId);
        const bool callerMatch = !swap.to || (caller >= swap.from && caller < swap.to);
        if (!idMatch || !callerMatch || !playRotk(swap.rotk, swap.what, id)) continue;
        cpu->R[0] = 0;  // no Two Towers handle
        return returnToCaller(cpu);
    }
    return 0;
}

void reset()
{
    gba_mod_set_function_hook_enabled(kHookId, 1);
    for (const auto& [id, clip] : g_state.clips)
        if (clip != GBA_MOD_AUDIO_CLIP_INVALID) gba_mod_audio_set_enabled(clip, 1);
}

bool envOn(const char* name)
{
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

}  // namespace

void rotkSoundsHeroSwing(int hero)
{
    if (!g_state.swap || hero < 0 || hero >= 6) return;
    playRotk(g_state.heroSwing[hero], "chained swing", 0xFF);
}

void rotkSoundsRegister()
{
    g_state.swap = envOn("ROTK_TTT_ROTK_SOUNDS");
    g_state.trace = envOn("ROTK_TTT_TRACE_SFX");
    if (!g_state.swap && !g_state.trace) return;
    if (g_state.swap) {
        const char* path = std::getenv("ROTK_TTT_ROTK_ROM");
        if (path) {
            std::ifstream file(path, std::ios::binary);
            g_state.rom.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        if (g_state.rom.size() < 0x00400000u) {
            std::fprintf(stderr, "[ttt] ROTK sounds: no ROTK ROM\n");
            g_state.swap = false;
        } else {
            g_state.swaps.assign(std::begin(kSwaps), std::end(kSwaps));
            for (std::uint32_t hero = 0; hero < 6; ++hero) {
                g_state.heroSwing[hero] = r16(kRotkHeroSounds + hero * 8);
                for (std::uint32_t k = 0; k < 4; ++k)
                    g_state.swaps.push_back({kTttHeroSounds[hero][k], kPlayerTickFrom, kPlayerTickTo,
                                             r16(kRotkHeroSounds + hero * 8 + k * 2), "hero sound"});
            }
            for (const Swap& swap : g_state.swaps)
                if (clipFor(swap.rotk) == GBA_MOD_AUDIO_CLIP_INVALID) g_state.clips.emplace_back(swap.rotk, rotkClip(swap.rotk));
        }
    }
    if (const char* test = std::getenv("ROTK_TTT_TEST_SWAP")) g_state.testId = static_cast<std::uint32_t>(std::strtoul(test, nullptr, 16));
    gba_mod_register_function_entry_plugin(kHookId, kTttSfxPlay, 1, sfxPlayEntry);
    gba_mod_register_reset_callback(reset);
}

}  // namespace rotk
