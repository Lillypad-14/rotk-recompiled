# ROTK Recompiled

A native PC version of *The Lord of the Rings: The Return of the King* (GBA) and the PC edition of the
ROTK Expanded mod. The game's code is statically recompiled to native code with
[GBARecomp](https://github.com/mstan/gbarecomp). Then the GBA's limits are removed one engine service at a
time: memory, CPU budget, sprite and tile limits, screen size, saves.

You need your own copy of the game. No ROM, BIOS, game art or code generated from the ROM is part of this
repository or its releases.

## Editions

| Build | Input | Notes |
| --- | --- | --- |
| Vanilla PC (`--target stock`) | Your `rotk.gba` (SHA-1 `ec3d126d37ea6e2ad7030d8a99cf87c6e46c03fd`) | The original game with the PC engine upgrades |
| Expanded PC edition (`--target expanded`) | The mod built from `game/` against your `rotk.gba` | Every mod version from here on is PC-only |
| GBA edition | [ROTK-Expanded-experimental](https://github.com/Lillypad-14/ROTK-Expanded-experimental) | The finished GBA-compatible mod. It also runs here as a ROM |

## What is already different from a GBA

- 64 MiB of extra RAM at `0x12000000` (`GBARECOMP_HOST_RAM_MB`), and the game heap lives there (32 MiB by
  default, `ROTK_PC_HEAP_MB`) instead of about 246 KiB of EWRAM.
- Optional CPU overclock (`GBARECOMP_CPU_OVERCLOCK`, x4 in the launchers) so busy scenes stop slowing down.
- Details and measurements: [docs/ENGINE_LIMITS.md](docs/ENGINE_LIMITS.md), [STATUS.md](STATUS.md).
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
- No game code, data or art is included. Bring your own `rotk.gba`.
