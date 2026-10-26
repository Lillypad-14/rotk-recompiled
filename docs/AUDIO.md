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

# Plan: native HD audio mixer (not started)

ROTK mixes in software: ARM code in IWRAM (copied from the IwramStaticsImage at ROM 0x08FF9F98) resamples
8-bit samples with linear interpolation (16.16 position), applies volume, accumulates the voices and
feeds DirectSound at 32,768 Hz. It patches its own shift operands (0x03000B70..0x03000BCC), so it runs
in the interpreter, and it cost about 29% of the CPU in a 16-enemy fight on the GBA edition. Mixer
entries: 0x03000604, 684, 6A4, 728, 7C0, 8A4, 8C8, 9B0, B38.

Goal: a launcher feature "HD audio" (original mixer as fallback) that mixes the same voices on the
host at 48 kHz in float, with better interpolation and no 8-bit DirectSound quantization, through
GBARecomp's trusted mod PCM pull stream (docs/MOD_AUDIO.md in the framework), muting the guest FIFO
output while it is on. Voice and channel caps can be lifted as with the other limits.

1. Reverse-engineer the voice state the mixer reads (sample pointer, step, volume, pan, loop) and how
   the music/sfx driver sets it up.
2. Host mixer fed from that state, replacing the mixer entries with hooks.
3. Check: identical voice state through both mixers, compare the output numerically (audio is not
   covered by the frame hashes), then listen against mGBA.
