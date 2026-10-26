# Frame pacing and runtime compilation

The target is a steady presentation cadence at the game's native 59.7275 Hz (280,896 GBA cycles per frame), without skipping simulation frames. Extra RAM and a larger guest CPU budget do not guarantee host frame deadlines. The original gameplay animation cadence is separate from presentation pacing.

## Corrections in this pass

- Runtime recovery uses the toolchain selected by CMake instead of an absent `C:/msys64/mingw64/bin/g++.exe`. `GBARECOMP_HEAL_CXX` remains an explicit override.
- The runtime DLL interface now exposes the resume-PC field required by generated code. Its ABI is version 7, so older cached DLLs are not reused. Spawn errors retain their actual Windows error details.
- `RotkRuntimeCompilerCheck` compiles, loads and executes authored ARM and Thumb functions, checks interior resume and cached execution, and optionally compiles/loads the three guest addresses reported in the player's screenshot. Loading those guest functions does not independently qualify their full gameplay behavior.
- The wide renderer decodes ordinary center-view text tiles by spans, draws nearer text backgrounds first, and skips texels that cannot enter the nearest two pixel candidates. Arbitrary coordinate providers and margin pixels retain their original sampling behavior. HD rendering retains its full submission stream. A 320-scene synthetic differential test retains the original pixel hash `ce715c1d44e9a64a`; its measured render time fell from about 506 ms to 379 ms across these changes. This is a compositor sample, not a whole-game speed claim.
- Completed frames wait for their presentation deadline before display. Previously the wait came after display, exposing alternating game-tick costs as short/long visible frames.
- Windows registers the emulation/presentation thread with MMCSS's Games task; compiler workers retain ordinary priority. Renderer VSync is explicitly set to the requested policy so fallback renderers cannot add a second limiter.

Native RAM opcode guards also take a direct contiguous RAM view instead of entering the general cartridge/mirror mapper on every instruction, and use explicit two-/four-byte loads instead of a variable-width shift loop. Opcode bytes are still validated live; nothing is cached across writes. `RotkCodeViewCheck` compares both access paths across mirrors, boundaries, writes and bus rebinding, including masked and unmasked instruction checks.

These changes are captured in `runtime-compiler.patch`, `wide-text-span.patch`, `frame-pacing.patch` and `live-ram-view.patch`. The original per-pixel compositor remains available under `GBARECOMP_SCANLINE_IMPLEMENTATION=LLE`.

## Timing evidence and limitations

The isolated fixture is `config/scenarios/widescreen-audit.toml`: 284 px, x4 guest CPU, HD audio on, smooth motion off. Tests use muted SDL dummy devices, private saves/settings and the same first-map checkpoint. Frame-phase recording is enabled and expensive diagnostic replay recording is disabled. Results vary with host activity and power/scheduling conditions; these are local samples, not a hardware-wide guarantee.

| Sample | Result |
| --- | --- |
| 900-frame baseline | About 56.6 FPS overall; 135 present gaps over 25.11 ms after 120 warm-up frames |
| 900-frame pacing correction | 45 gaps over 25.11 ms; still below target |
| 900-frame MMCSS sample | About 59.66 FPS overall; 59.72 Hz after warm-up; 0 long gaps or missing guest frames; 4 work-budget overruns |
| 1,800-frame repeat | About 56.3 FPS; 88 long gaps after warm-up; the short result did not establish sustained qualification |
| Existing 400-enemy stress checkpoint, 300 frames | About 16.5 FPS; not qualified for steady play |
| Cold 64-enemy exploratory run | Timed out at 150 seconds; not a pass or a paced FPS measurement |
| Final foreground-culling build, 1,800-frame normal scene | 59.726 Hz after warm-up; 0 long gaps, work-budget overruns or missing guest frames |
| Final foreground-culling build, 1,800-frame movement/action replay | 59.729 Hz after warm-up; 0 long gaps, work-budget overruns or missing guest frames |
| Updated 400-enemy checkpoint, 600 frames | About 59.08 FPS overall; 0 long gaps/missing frames, but 25 work-budget overruns after warm-up; still insufficient headroom for a blanket guarantee |
| Same 400-enemy checkpoint with live RAM guard optimization | 59.53 Hz after warm-up; 0 long gaps/missing frames, 10 work-budget overruns; same checkpoint image |
| Final rebuilt movement/action replay, 1,800 frames, live RAM guards enabled | 59.729 Hz after warm-up; 0 long gaps, work-budget overruns or missing frames; 0 HD-stream underflows; gameplay screenshot inspected |

HD stream counters reported zero underflows and zero dropped stream frames in the completed paced normal-gameplay samples. That does not prove every audible click is fixed. Real-device listening, cold first encounters, more rooms/characters/effects and sustained fights still need qualification. CPU coverage remains `NOT_STATIC`; cache-healed code is distinct from ahead-of-time coverage.

## Reproduce and evaluate

```powershell
cmake --build build/host-expanded --target RotkRuntimeCompilerCheck RotkWideRenderCheck --parallel 2
./build/host-expanded/RotkRuntimeCompilerCheck.exe .private/compiler-check .private/expanded/rotk.gba
./build/host-expanded/RotkWideRenderCheck.exe
./build/host-expanded/RotkCodeViewCheck.exe
python -m unittest discover -s tests -p test_frame_report.py
```

Use `tools/check.py --dummy-window --mods-state config/scenarios/widescreen-audit.toml --load-state <private checkpoint> --frames 1800 --target expanded`. `--executable` permits an isolated experimental build without replacing the normal executable.

`tools/frame_report.py <run>/frame-phase.csv --max-long-gaps 0` fails if any present interval exceeds 1.5 native periods or guest frame numbers skip/repeat. It also reports p95/p99/max intervals and useful work exceeding the 16.74 ms budget. Its long-gap gate is explicitly not a measurement of monitor scanout or a universal zero-drop guarantee. Startup is reported separately through the configurable warm-up count.

## Cross-module optimization experiment

`ROTK_THIN_LTO=ON` enables Clang Release interprocedural optimization for the generated cartridge and CPU/device/runtime libraries. It preserves the non-LTO build as a comparison. The linker is limited to two ThinLTO jobs to bound memory use. Enable it only with behavior and timing checks; no benefit is inferred merely from successful compilation.

The isolated ThinLTO build preserved the 1,800-frame checkpoint image but measured about 57.86 FPS with 23 long gaps after warm-up. It remains off by default; the final successful regular build uses the foreground-culling renderer. A separate legacy BIOS-HLE experiment also preserved that image but ran slower (about 50.2 FPS), and was not promoted. These experiments were not compared under controlled thermal conditions, so they do not establish universal rankings of the backends.

## Per-instruction DMA/IRQ fast path (2026-10-26)

The runtime now skips the DMA drain helper when no stolen cycles are pending, and inlines the live IE/IF/IME interrupt predicate. It still checks IRQ eligibility after each instruction; pending transfers retain the same clock/device advancement. This does not reduce enemy updates, animation, collision checks or guest timing. The reproducible framework change is `patches/hot-tick.patch`.

`RotkTickFastPathCheck` verifies live interrupt masking/acknowledgement, high-byte DMA IRQs, immediate DMA copying and non-destructive cycle-debt probing. The framework DMA, timer and bus suites also pass.

The same 400-enemy checkpoint over 600 presented frames measured 59.609 Hz after 120 warm-up frames, zero long gaps/missing frames and eight work-budget overruns (previous sample: 59.531 Hz and ten overruns). Its final image matched exactly. This small sample is consistent with reduced overhead, but does not establish a reliable percentage improvement or eliminate crowded-fight overruns. A separate 1,800-frame movement/action replay measured 59.730 Hz, zero long gaps, missing frames or work-budget overruns, and the same final image. Both runs reported zero HD-stream underflows/dropped frames. Measurements use paced dummy presentation, not monitor scanout. Coverage remains NOT_STATIC.

Build the focused test with `cmake --build build/host-expanded --target RotkTickFastPathCheck --parallel 2`, then run `build/host-expanded/RotkTickFastPathCheck.exe`.

## Active crowd profiling and yield fast path (2026-10-26)

`config/scenarios/expanded-crowd-actions.csv` adds a repeatable 1,800-frame movement/A-press workload starting at frame 5150 of the private 400-enemy checkpoint. This is an exploratory stress sequence, not verified spell coverage or a representative campaign encounter. Its final gameplay screenshot was inspected. It is substantially heavier than the earlier idle crowd sample, so those FPS figures are not directly comparable.

A separate Windows main-thread sampling run found `runtime_should_yield` (247 samples), `runtime_tick_impl` (242), live RAM instruction guards (155 combined), and the RAM code-view callback (80) among the main costs in 1,634 executable samples. Sampling suspends only the tool's owned child thread; it is not enabled in timing runs.

`patches/yield-fast-path.patch` separates the ordinary yield predicate from its existing slow path. BIOS prefetch, mutable-code hooks, pending VBlank processing, quit, watchdog, breakpoints and multiplayer still enter the original logic. HALT remains live. The generated game code and guest instruction timing are unchanged. On Windows Clang the common function saves two registers instead of seven; this is evidence of removed host overhead, not an FPS guarantee.

Two sequential baseline action runs measured 39.055 and 43.367 Hz after warm-up, versus 47.683 and 44.252 Hz with the fast path. Work-budget overruns were 1,546/1,515 before and 906/1,033 after; long presentation gaps remained in every run. Another running game instance was discovered during qualification, so host load was not controlled and these samples do not establish a dependable gain percentage. The preliminary run that overlapped sampling is excluded from this comparison. All compared runs reported the same three RAM dispatch misses and 243,569 interpreted instructions; coverage remains NOT_STATIC.

Both fast-path snapshots matched the baseline byte for byte except for the eight-byte host-seconds field of the inactive cartridge RTC. CPU state, guest memories (including used Host RAM), IO, audio, save, PPU and runtime metadata matched. Final image SHA-256: `ecdb2c5a11c3f4de4024a7d559dafa9b4d216fdd063cb990f39bb4fc12e97a43`. The first clean baseline and first fast-path run each reported one HD audio-stream underflow: audio and sustained fight cadence remain unfinished.

The expanded focused test checks HALT, matching/unmatched debug breakpoints, mutable-code hook yields and VBlank deferral inside nested IRQs in addition to DMA/IRQ behavior. Native multiplayer session tests pass. Both stock and Expanded executables compile. The build wrapper's dependency staging encountered a locked DLL in the running game instance; that instance was not stopped or modified.

The final normal movement replay retained image `8ace83e3f549df670bbcf5ba8c5e6aae88039887f6e8aa4b85fb360092e81bb2`, with zero missing frames and zero audio-stream underflows. Under the concurrent load it measured 59.349 Hz, three long gaps and 64 work-budget overruns. The previous build was also rechecked under that load: 59.166 Hz, 4 long gaps and 76 overruns. These loaded runs do not supersede the earlier isolated qualification or establish a normal-gameplay regression.
