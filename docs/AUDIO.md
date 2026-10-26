# Desktop audio correction

The stock game's live `audio_state` at frames 299 and 899 reports SOUNDBIAS `0x0200`, a 32,768 Hz mixer rate, and two samples per event. Evidence is retained locally in `.private/audio-investigation/audio-state.json`.

The old desktop runner discarded sample-rate metadata and passed raw PCM to an output path fixed at 65,536 Hz. That gives 32,768 Hz game audio the wrong playback duration and pitch, independently of whether the guest sound driver executes correctly. The browser runner already used rate-tagged blocks.

`patches/desktop-audio-rate.patch` makes the desktop runner drain tagged blocks as well. SDL converts each non-65,536 Hz segment to the existing bridge's 65,536 Hz format, retaining conversion state across blocks. Rate changes flush/recreate the converter. Host mod clips are mixed after this conversion because they use a fixed 65,536 Hz format. No ROM-derived generated code or guest driver is replaced.

`tools/lab.py prepare` and `build` apply this local patch idempotently to the pinned framework. Conflicts fail for review. The framework checkout intentionally has these three source modifications; its upstream commit hash alone is no longer the complete build identity. Preserve the patch with the PC project.

## Validation

- Existing `audio_playback_tests` pass (rate boundaries, partial drains, snapshot restore, discard).
- Existing `audio_drc_tests` pass.
- `RotkAudioRateCheck` feeds a synthetic 700 Hz tone in odd-sized blocks at all four hardware rates. It checks duration and pitch after SDL conversion, without using a ROM or physical audio device. SDL2 retains up to 1,024 output samples of startup/tail latency on this SDK; the check allows that measured bound rather than claiming sample-exact segment transitions.
- A muted SDL dummy-device window run completes 900 presented frames at approximately 59.8 guest FPS. At 840 pushes the fixed output probe reports approximately 14.1 seconds of PCM, consistent with its frame duration. Dummy-device queue timing is not a real-device underrun benchmark.
- The window run still reports NOT_STATIC coverage: four dispatch misses and 1,765,161 interpreted instructions. This playback fix does not resolve CPU coverage.

Build/run the focused check:

```powershell
cmake --build build/host --target RotkAudioRateCheck audio_playback_tests audio_drc_tests --parallel 4
./build/host/RotkAudioRateCheck.exe
./build/host/framework/audio_playback_tests.exe
./build/host/framework/audio_drc_tests.exe
```

Normal-device listening, song/effect accuracy against mGBA, extended gameplay, and save/load audio transitions remain unqualified. Other timer, FIFO, CPU-timing or sound-driver translation bugs could remain. Use `Play.cmd` to assess the correction with normal audio.

# HD audio mixer (launcher feature "HD audio", src/hd_audio.cpp)

Off by default. On, the host replaces the game's software mixer, advances the same voice state and
renders it in float at 65,536 Hz (4x the game's rate) with cubic, linear or original interpolation,
then hands it to GBARecomp's mod PCM pull stream (framework docs/MOD_AUDIO.md). The guest output ring
is zero-filled, so the FIFOs play silence. Off, the original mixer runs untouched.

## How ROTK mixes (stock ROM; same addresses in Expanded)

The sound library (Thumb 0x0804AA80..0x0804DC44 plus ARM code in IWRAM, copied from 0x08FF9F98) has no
known engine signature.

- Hardware (0x0804AAA0): TM0 reload 0xFC00 = **16,384 Hz** mix rate (SOUNDBIAS's 32,768 Hz is only the
  DAC rate). TM1 cascades and IRQs once per 1,104-sample ring (IWRAM 0x03000004 restarts DMA1/2).
  SOUNDCNT_H 0x9A0C: FIFO A left, FIFO B right, both 100%. The PSG is unused.
- Rings: s8 L at 0x02000000, R at 0x02000450, 4 chunks of 276 samples.
- `sfx_libVBlank` -> 0x03001080 asks 0x0804AB80 for the next chunk (276) and calls
  **`sfx_mixFrame(outL, outR, count)` = 0x03000C48**, splitting the chunk at the music tick
  (callback at 0x02001638, countdown 0x0200163E, reload 0x0200163C). Counts are always multiples of 4.
- Voices: 32 x 44 bytes at 0x020008B4. +0 vol, +1 pan (-64..64), +2 state (1 playing), +3 loop mode
  (0 one-shot, 1 loop, 2 ping-pong), +4 start, +8 ptr, +12 end, +16 loop length, +20 step (16.16,
  negative while a ping-pong voice runs backwards), +24 handle (index | serial << 8), +28 frac,
  +30/+31 L/R volume, +32 interpolation (0 or 8), +33 selector, +34 sample flags, +36 category (release:
  ramp samples left), +40 loop start (release: u16 level 12.4), +42 (release: step).
  Per-voice setters in IWRAM: 0x03000094 play, 0x03000320 stop/release, 0x030003E8 pitch,
  0x03000434 volume, 0x030004B4 pan, 0x03000578 position, 0x030005C8 is-playing.
  Category volumes at 0x03000090, stereo flag 0x03000000, interpolation mode 0x03000092.
- Routine table 0x08280D10 (index selector + interpolation): 0/2 stereo 0x03000728, 1 left 0x03000604,
  3 right 0x03000684, 4 centre (left volume) 0x030006A4, 7 release ramp 0x0804AF78 (ARM in ROM, 16
  samples from the last level to the centre). +8: the linear-interpolated versions 0x030009B0, 7C0, 8A4,
  8C8. Samples are unsigned 8-bit.
- mixFrame clears the accumulator (0x03000AFC: 0x0300115C, words L01 L23 R01 R23 per 4 samples), mixes
  each playing voice in spans that end where it loops, bounces or stops (span length via BIOS Div,
  rounded up to 4), advances silent voices without mixing (0x03000F14), fills stopped voices with
  centre bias (0x03000AB4), then converts (0x03000B38): subtract the bias, shift (patched by
  0x03000BF0, 4 in the stock game), clip table at 0x02001238 (gain 0x80, 0x0804ACF0).
  Net result: `out_s8 = clip(sum((sample - 128) * vol / 128))`.
- The GBA mod's Mute writes 0xEA000091 at 0x03000CC8; the host port honours it. Voice Cap only limits
  allocation, Fast Mixer only skips the guest blend.

## Checks (2026-10-26)

- `ROTK_PC_HD_AUDIO=verify` lets the guest mixer run and compares the host port's predicted voice
  state at the next call. Stock first-map 7,500 frames: 422,121 voice comparisons, 0 mismatches
  (70 driver stops between calls are classified separately). Expanded 8,800 frames: 458,993, 0.
- Same frame hash with the GBA mixer, verify and replace: stock `824b55b9...`, Expanded `61a4a6b4...`
  (both with the launcher state of that day, CPU x4). Interpreted instructions on the route: stock
  162.6M -> 1.5M, Expanded 156.0M -> 0.23M. The mixer was nearly all of the remaining interpreted work.
- Output: `ROTK_PC_HD_AUDIO_INTERP=guest` + `ROTK_PC_HD_AUDIO_WAV` / `ROTK_PC_HD_AUDIO_GUEST_WAV` dump
  both mixes. Every 4th HD frame vs the guest ring: 98.7% within 1 s8 LSB, worst 3.2 (the guest
  truncates per voice and its interpolation adds 1 after a falling step). Cubic: same loudness (rms
  18.2 vs 17.8 s8).
- Package path: feature enabled in a copied mods/state.toml, windowed run on SDL's dummy audio
  driver, 2,400 frames: stream pulled everything, 0 dropped frames, 1 underflow.
- Not checked yet: listening on a real device, savestate load, fast-forward, pause.

## Env

`ROTK_PC_HD_AUDIO` = 0 | 1 | verify (overrides the package), `ROTK_PC_HD_AUDIO_INTERP` = cubic | linear
| guest, `ROTK_PC_HD_AUDIO_WAV` (HD dump, 65,536 Hz stereo; also lets env mode 1 replace without the
package stream), `ROTK_PC_HD_AUDIO_GUEST_WAV` (verify only, 16,384 Hz), `ROTK_PC_HD_AUDIO_DEBUG`.

## Stereo output (patches/stereo-audio.patch)

The recomp used to play mono end to end. Now the desktop device opens in stereo by default
(`GBARECOMP_AUDIO_STEREO=0` keeps mono):

- `GbaAudio` keeps the mono stream bit-identical as the mid signal (L+R)/2 and stores the side
  (L-R)/2 in a parallel ring (PSG per-side routing and master volumes, FIFO A/B routing, MP2K shadow).
  `drain_sample_block_stereo` rebuilds L/R; the mono drain still serves web, netplay and tools.
- Host window: 2-channel device, stereo rate conversion and bridge (RAB already supported 2 channels),
  mono producers copied to both channels. The web host and stub builds downmix.
- Mod audio: `gba_mod_audio_stream_register_s16_stereo` and `gba_mod_audio_mix_stereo`; fixed clips
  play centred, a stereo stream is averaged on a mono device. HD audio uses the stereo stream.
- Checks: framework audio_playback, audio_drc and mod_audio tests, RotkAudioRateCheck pass. SDL's disk
  audio driver captured the real device stream, 2,400 frames of the first map: L/R correlation 0.735
  with the GBA mixer and 0.735 with HD audio (mono would be 1.0), same levels.
- tools/lab.py now treats the patches as a stack (top applied = all applied), since this patch edits
  lines the desktop-audio-rate patch touched.

## Next

1. Lift voice limits: the 32-voice table is fixed in EWRAM; more voices need a host table and the
   setters above hooked.
2. Optional: smooth volume changes across calls (the driver steps volumes per tick).
