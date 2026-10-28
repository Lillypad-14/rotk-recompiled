# Making a release download

A release is one Windows zip: both executables, the Expanded patch and the files they load. It holds
no ROM, BIOS or game art; the player picks their own ROM and BIOS in the launcher.

## Build

1. The Expanded build and its patch, from the mod's current PC edition
   ([EXPANDED_EDITION.md](EXPANDED_EDITION.md)):

   ```powershell
   make -C game hack-pc
   python tools/lab.py prepare --target expanded --rom game/build/rotk_pc.gba --elf game/build/hack_pc.elf
   python tools/lab.py build --target expanded
   python tools/make_edition_patch.py      # prints -DROTK_EDITION_SHA1=...
   ```

2. The vanilla build with that patch. `ROTK_EDITION_EXE` must be relative so the zip works anywhere:

   ```powershell
   cmake -S . -B build/host -DROTK_EDITION_PATCH=<bps> -DROTK_EDITION_SHA1=<sha1> -DROTK_EDITION_EXE=ROTK-Recompiled-Expanded.exe
   python tools/lab.py build --target stock
   ```

   Both release executables are Windows GUI programs, so players get no console window:
   configure `build/host` and `build/host-expanded` with `-DROTK_GUI_SUBSYSTEM=ON` before building
   them. Turn it back off for development (`Play.ps1` and the test tools wait on the console).

3. Check the patch: `build/host-expanded/RotkBpsPatchCheck.exe <unmodified rom> build/host/expanded.bps`
   must print the Expanded build's ROM SHA-1 (`rom_sha1` in `.private/expanded/provenance.json`).

## Package

The zip holds one folder, `ROTK-Recompiled/`, with:

| From | Files |
| --- | --- |
| `build/host` | `ROTK-Recompiled.exe`, `SDL2.dll`, `libc++.dll`, `libunwind.dll`, `libwinpthread-1.dll`, `expanded.bps`, `launcher-window.ini`, `assets/` |
| `build/host/mods` | `packages/` only |
| `build/host-expanded` | `ROTK-Recompiled-Expanded.exe` |
| `external/gbarecomp/tools/fetch_tcc.ps1 -Toolchain <zip folder>\overlay_toolchain` | `overlay_toolchain/`: TinyCC and the overlay headers |
| repository | `LICENSE`, `patches/LICENSE.md`, `CREDITS.md` in `licenses/`, and a `PLAY.txt` |

`overlay_toolchain/` lets the game compile, on the player's PC, the functions its ahead-of-time
build missed (self-heal), cached in `recomp_cache/` for the next start. Without it the game still
plays, on the interpreter, but prints a "compile FAILED" line for every missed function. TinyCC is
LGPL 2.1: ship its licence text with the source link in `licenses/TinyCC-LGPL-2.1.txt`.

Never package `build/host`'s `mods/state.toml`, `config.ini`, `rom.cfg`, `bios.cfg`, `diagnostics/`,
`recomp_cache/` or saves: they are the maintainer's own settings and paths.

## Test before publishing

Unzip into an empty folder and run both executables there with only a ROM and a BIOS:

```powershell
.\ROTK-Recompiled.exe --no-launcher --rom <rom> --bios <bios> --no-window --frames 600
.\ROTK-Recompiled-Expanded.exe --no-launcher --rom <rom>-expanded.gba --bios <bios> --no-window --frames 600
```

Then open the launcher, pick the ROM and BIOS, tick Expanded and press Play.

Publish as a GitHub release (pre-release while experimental) with the zip attached. GBARecomp is
PolyForm Noncommercial, so the download may only be shared for free.
