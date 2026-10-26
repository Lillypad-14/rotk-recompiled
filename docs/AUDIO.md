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
