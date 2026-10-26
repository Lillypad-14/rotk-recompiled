# PC bugfix pass — 2026-10-26

This pass addresses widescreen map-edge corruption and HD-audio starvation clicks. It does not qualify the entire port or resolve all frame-pacing issues. Existing uncommitted smooth-motion/transparency work and launcher preferences were preserved.

## Widescreen

The horizontal camera clamp wrote a supposed second camera value to `0x03005880 + player * 8`. That address is actually the BG3 tile staging buffer. On a clamp it replaced tilemap entries with the camera coordinate. The camera still clamps, but the extra staging-buffer write is removed.

`RotkWidescreenEdgeCheck` exercises the registered hook against a real bus and canary tile entries, at both horizontal boundaries for both camera slots and five view margins. The preservation assertion failed before the correction and passes afterward. It also checks margin tile lookup and invalidation across scanlines/frames.

Adjacent margin pixels now reuse their metatile lookup within one scanline. The cache is invalidated across scanlines and every published frame, keeping live tile changes visible. This removes redundant memory reads; no FPS improvement is claimed from the available measurements.

## Audio

The HD stream previously transitioned immediately between nonzero PCM and silence on starvation/restart. It now fades over 128 output frames (about 1.95 ms), and primes two mixer chunks instead of one before starting/restarting. The additional priming cushion is about 17 ms. The healthy buffered signal remains at unity gain; the guest voice state and mixer calculations are unchanged.

`RotkAudioEnvelopeCheck` verifies bounded sample steps for a synthetic constant stereo signal through startup, depletion, silence and restart, plus unity gain during steady playback. This specifically checks discontinuities caused by buffering; it cannot establish song/effect accuracy or eliminate every possible pop.

## Runs and limits

Both stock and Expanded executables build successfully. Evidence stays under `.private/`.

| Run | Result |
| --- | --- |
| Expanded first-map replay, 8,800 frames, smooth motion off | Completes; gameplay screenshot inspected |
| Paced silent SDL window, 2,400 presented frames | Completes; HD stream reports 0 underflows and 0 dropped frames |
| Isolated widescreen gameplay checkpoint, 600 presented frames, 284 px, x4 CPU, smooth motion off | Completes; HD stream reports 0 underflows and 0 dropped frames; approximately 57.6 FPS |
| Extended walking/attack replay, 11,400 frames, native HD mixer | Completes; however launcher preferences changed to native width during this run, so it does not qualify widescreen walking |
| Same extended replay with guest mixer verification | Times out at 120 seconds; not a pass |

The timing runs use SDL dummy devices and do not establish real-device listening quality. Some checks ran alongside other activity. Approximately 57.6 FPS leaves frame pacing unresolved. CPU fallback remains present (`NOT_STATIC`); no fully native coverage or mGBA equivalence is claimed.

## Reproduce

```powershell
cmake --build build/host-expanded --target RotkWidescreenEdgeCheck RotkAudioEnvelopeCheck --parallel 4
./build/host-expanded/RotkWidescreenEdgeCheck.exe
./build/host-expanded/RotkAudioEnvelopeCheck.exe
python tools/check.py --target expanded --name paced --frames 2400 --dummy-window --mods-state config/scenarios/widescreen-audit.toml --input config/scenarios/expanded-first-map.csv
```

`--mods-state` copies the executable, DLLs, package manifests and feature-state fixture into the run directory. The normal launcher's settings and player saves are untouched. `--dummy-window` exercises desktop audio/video pacing without audible playback and exports frame-phase timings. `--load-state` can start this mode from a copied checkpoint; completion uses presented frames rather than an absolute restored PPU count.

Play the updated Expanded build with `PlayExpanded.cmd`. Next priorities are a reproducible full widescreen edge-walking run, busy-fight frame-time profiling, real-device listening, and pause/save/load/fast-forward audio transitions.
