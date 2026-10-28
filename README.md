# ROTK Recompiled

A native PC version of *The Lord of the Rings: The Return of the King* (GBA) and the PC edition of the
ROTK Expanded mod. The game's code is statically recompiled to native code with
[GBARecomp](https://github.com/mstan/gbarecomp). Then the GBA's limits are removed one engine service at a
time: memory, CPU budget, sprite and tile limits, screen size, saves.

You need your own copy of the game. No ROM, BIOS or game art is part of this repository or its releases:
the game runs from your own ROM and GBA BIOS dump.

## Download and play (Windows)

1. Download `ROTK-Recompiled-<version>-windows.zip` from the
   [releases](https://github.com/Lillypad-14/rotk-recompiled/releases) and unzip it anywhere.
2. Run `ROTK-Recompiled.exe` and pick your ROM (*The Return of the King*, GBA, USA/Europe, SHA-1
   `ec3d126d37ea6e2ad7030d8a99cf87c6e46c03fd`) and your GBA BIOS dump (SHA-1
   `300c20df6731a33952ded8c436f7f186d25d3492`).
3. Tick **Expanded** for the Expanded mod, or leave it off for the original game, and press Play.

No build tools are needed. To build it yourself, see [Building](#building).

## Editions

| Build | Input | Notes |
| --- | --- | --- |
| Vanilla PC (`--target stock`) | Your `rotk.gba` (SHA-1 `ec3d126d37ea6e2ad7030d8a99cf87c6e46c03fd`) | The original game with the PC engine upgrades |
| Expanded PC edition (`--target expanded`) | The mod built from `game/` against your `rotk.gba` | Every mod version from here on is PC-only |
| GBA edition | [ROTK-Expanded-experimental](https://github.com/Lillypad-14/ROTK-Expanded-experimental) | The finished GBA-compatible mod. It also runs here as a ROM |

## Launcher and options

The game opens a launcher first (GBARecomp's recomp-ui). Its **Mods** page lists the built-in
"ROTK PC Enhancements" package. Each enhancement below can be switched off or tuned there, and with all
of them off the game runs exactly like the original. Environment variables override the launcher for
tests and scripts. Headless runs (`--frames`, `--no-window`, `--tcp`) skip the launcher.

## Expanded and The Two Towers

The launcher's front page has an **Expanded** checkbox: on, Play patches your unmodified `rotk.gba`
into the Expanded mod (a copy beside it; the original is never changed) and plays that
([docs/EXPANDED_EDITION.md](docs/EXPANDED_EDITION.md)).

The Expanded edition can also use *The Two Towers* (GBA, USA/Europe). Put your Two Towers ROM, and its
`.sav` if you want your heroes, in the same folder as your `rotk.gba`. Then:

- **Import a Two Towers hero:** on the Load Game menu, press L on an empty slot. The hero arrives with
  their level, items (with their Two Towers names, stats and icons) and skill levels.
- **Skill page 3, The Two Towers:** Frodo, Legolas, Aragorn, Gandalf, Eowyn and Gimli get their Two
  Towers skills, bought with skill points by The Two Towers' rules. Imported heroes keep their levels.
  Work in progress: every hero has their passives; so far only Gandalf has his actives, two of
  them still ROTK stand-ins.
- **Mod Options > Two Towers > Legacy Skills** turns the skills and the page off.

## What is already different from a GBA

- 64 MiB of extra RAM at `0x12000000` (`GBARECOMP_HOST_RAM_MB`), and the game heap lives there (32 MiB by
  default, `ROTK_PC_HEAP_MB`) instead of about 246 KiB of EWRAM.
- Optional CPU overclock (`GBARECOMP_CPU_OVERCLOCK`, x4 in the launchers) so busy scenes stop slowing down.
- Actor pools of 1,024 main and 160 overlay slots instead of 256 and 40 (`ROTK_PC_ACTORS`,
  `ROTK_PC_OVERLAY_ACTORS`), with the per-actor collision and render queues sized to match. Tested with
  more than 256 actors on screen.
- Up to 1,024 sprite pieces per frame instead of the GBA's 128 OAM entries (`ROTK_PC_EXTRA_OBJECTS`).
- The engine's draw-order sort works past 256 queued actors (the original counted them in a byte).
- Optional widescreen (16:10, 16:9, 2:1, 21:9): more of the level at the sides, camera kept inside
  the level, enemies in the margins active. Optional HUD at the screen edges. Works with smooth motion,
  and in-game dialogue stays wide; menus and cutscenes keep their width with black bars. See
  [docs/WIDESCREEN.md](docs/WIDESCREEN.md).
- With these at their stock values the game renders exactly like the original (same frame hashes).
- HD audio: the game's mixer runs on the PC at 65 kHz in float, on by default ([docs/AUDIO.md](docs/AUDIO.md)). Sound is now stereo, as on the GBA.
- Smooth motion: halfway frames between the game's 30 Hz ticks, on by default ([docs/SMOOTH_MOTION.md](docs/SMOOTH_MOTION.md)).
- Details and measurements: [docs/ENGINE_LIMITS.md](docs/ENGINE_LIMITS.md), [STATUS.md](STATUS.md).
  Current frame-time measurements, compiler repairs and remaining workload limits: [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
  Next steps: [docs/ROADMAP.md](docs/ROADMAP.md).

## Building

Requirements: Python 3.11+, Git, CMake, Ninja, Clang/LLVM-MinGW, SDL2 development files, your `rotk.gba`
and a GBA BIOS dump (SHA-1 `300c20df6731a33952ded8c436f7f186d25d3492`). The Expanded edition also needs
the mod's build tools (see `game/docs/setup.md`).

```powershell
git clone --recursive https://github.com/Lillypad-14/rotk-recompiled.git
cd rotk-recompiled

# Vanilla
python tools/lab.py prepare --rom path\to\rotk.gba --bios path\to\gba_bios.bin --sdl2 path\to\SDL2
python tools/lab.py build --sdl2 path\to\SDL2
.\Play.cmd

# Expanded PC edition: build the mod in game/ first (it expects game/rotk.gba), then
python tools/lab.py prepare --target expanded --bios path\to\gba_bios.bin
python tools/lab.py build --target expanded --sdl2 path\to\SDL2
.\PlayExpanded.cmd
```

Rebuild the Expanded target after every mod build: the executable is tied to that exact ROM.

## Checks

`python tools/check.py [--target expanded] --name NAME --frames N [--input config/scenarios/...]` runs a
muted, headless checkpoint and keeps a screenshot, state dump, coverage and log under `.private/runs`.
`RotkHostRamCheck` covers the host RAM region.

## Layout

- `src/`: the game host and native engine replacements.
- `patches/`: changes to GBARecomp, applied in order on top of the pinned revision in `framework.lock.json`.
- `config/`: recompiler settings per target and input scenarios.
- `tools/`: prepare/build (`lab.py`) and checkpoints (`check.py`).
- `external/gbarecomp`: the framework (submodule). `game`: the decompilation and the Expanded mod (submodule).

## Credits and licences

See [CREDITS.md](CREDITS.md).

- This repository's own code (host, tools, configuration, docs): [MIT](LICENSE).
- `patches/` modifies GBARecomp and stays under its PolyForm Noncommercial License 1.0.0
  ([patches/LICENSE.md](patches/LICENSE.md)). Executables built from this project include GBARecomp, so
  they can only be shared noncommercially.
- No ROM, BIOS, game data or art is included. Bring your own `rotk.gba` and GBA BIOS. The release
  executables contain the game's code statically recompiled from the ROM, as other recompilations do.
