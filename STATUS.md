# Measured status — 2026-10-25

This is an isolated stock-ROM experiment. The Expanded ROM, main decompilation checkout, previous recomp build, and their saves are unchanged.

## Built and observed

- Pinned GBARecomp revision: `9266573123fb950dfdee2d19040a2daf5e63e39d`.
- Imported 1,059 function roots from the current decompilation configuration.
- Regenerated 14,000 cartridge functions across 16 shards and 770 BIOS functions.
- Release build succeeds with LLVM-MinGW, Ninja and SDL2. Interactive launcher: `Play.cmd`.
- A cold 900-frame run completes and visibly reaches language selection.
- The 3,600-frame `config/scenarios/start.csv` run completes and visibly reaches the opening Aragorn/Saruman dialogue.

| Checkpoint | Result | Execution coverage |
| --- | --- | --- |
| Cold boot, 900 frames | Passed; language menu screenshot | NOT_STATIC; 4 distinct misses, 1,762,305 interpreted instructions |
| Menu navigation, 3,600 frames | Passed; opening cutscene screenshot | NOT_STATIC; 23 distinct misses, 76,013,898 interpreted instructions |
| Extended navigation, 7,500 frames | Passed; gameplay terrain, player and HUD visibly rendered | NOT_STATIC; 33 distinct misses, 154,703,495 interpreted instructions |
| Strict native boot, 900 requested frames | Failed at ARM entry `0x03000C48` | No fallback permitted; no strict native baseline established |
| Interpreter-only boot | Timed out at 180 seconds | No completed comparison or screenshot |
| Interpreter-only boot without state export | Timed out at 35 seconds | State export alone does not explain the incomplete run |

Run evidence is local under `.private/runs`, including logs, result JSON, and screenshots/state dumps for completed checkpoints. The language-menu PNG SHA-256 is `88142e65939771e0821ec6359d1ec2dea4ba7e2f06d4db3901a971397bc882c4`.

## Remaining work

Desktop playback now preserves the game's actual mixer rate, correcting the 32,768-to-65,536 Hz mismatch. The local framework source patch and validation are documented in [AUDIO.md](docs/AUDIO.md). This is a host playback correction, not a replacement or qualification of the game's sound driver.

The normal executable uses generated native code with interpreter recovery. Boot misses include ARM entries `0x0300004C`, `0x03000B38`, `0x03000C48`, and `0x03000C80`. Some RAM code changes at runtime; generated entries alone do not establish safe coverage of modified bytes.

Runtime healing also attempts the framework's default Windows compiler at `C:/msys64/mingw64/bin/g++.exe`, which is unavailable here. Those attempts fail and execution stays on the interpreter bridge. A future controlled healing experiment should explicitly select a compatible compiler through `GBARECOMP_HEAL_CXX` and verify the resulting DLLs. Cached healing must still be reported separately from static coverage.

The `config/scenarios/first-map.csv` trace reaches a visually rendered gameplay map at 7,500 frames. Movement, combat, skills, transitions, and save/reload are not qualified. The input trace is exploratory, not an acceptance test. Interpreter-only and mGBA comparisons remain unresolved. Audio and interactive controls have not been manually qualified.

There is no measured claim of reduced in-game slowdown, increased RAM, more actors, widescreen, or removed GBA graphics limits. The runtime still models the original hardware. No engine service has been replaced yet.

## Engine limits (2026-10-25)

`patches/engine-limits.patch` adds 64 MiB of flat host RAM at `0x12000000` and an opt-in CPU overclock
(`GBARECOMP_CPU_OVERCLOCK`, `Play.cmd` uses x4). Details in [ENGINE_LIMITS.md](docs/ENGINE_LIMITS.md).

| Check | Result |
| --- | --- |
| Boot, 900 frames, host RAM present | Frame hash unchanged from the baseline above |
| First-map route, 7,500 frames, x1 and x4 | Same gameplay scene reached |
| `RotkHostRamCheck` | Passed (round trips, no mirroring, bounds, DMA rule, snapshot prefix) |
| Game heap in host RAM (32 MiB), boot and first-map | Frame hashes identical to the stock heap; heap high-water about 250 KB in host RAM |

Host RAM moved to `0x12000000` so BIOS copy/decompress calls accept it. The overclock's effect on lag
frames is not measured.

## Expanded mod build (2026-10-25)

The PC version is the Expanded mod; the GBA build is no longer a target. `python tools/lab.py prepare --target expanded`
recompiles `rotk/build/rotk_hack.gba` (32 MiB) with the stock roots plus the 918 FUNC symbols of
`rotk/build/hack.elf` and the reviewed entries in `config/rotk-expanded.extra.toml`. `build --target expanded` makes
`build/host-expanded/RotkPcLabExpanded.exe` (SRAM save, debug port 19853). `PlayExpanded.cmd` plays it.

| Check | Result |
| --- | --- |
| `config/scenarios/expanded-first-map.csv`, 8,800 frames | Save format, mod title menu, opening cutscene, gameplay with HUD |
| Heap | `[rotk] game heap: 32 MiB of host RAM` (the hook catches `modRam_init`'s `memory_addHeap`) |
| ROM coverage | 22 ROM misses on the first run, 0 after adding them as roots; 233 guest fps headless |
| Remaining misses | 21 IWRAM entries (self-modifying stock engine code, mostly the sound mixer); about 160 M interpreted instructions per 8,800 frames |

## Next milestone

Extend the first-map route with movement and combat, then compare it with mGBA. Investigate dispatch coverage using reviewed metadata and framework source changes, preserving a faithful reference build. Follow [the roadmap](docs/ROADMAP.md) before attempting Expanded support or engine replacement.
