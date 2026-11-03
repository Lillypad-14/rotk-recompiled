# The Expanded toggle

One game, one launcher: the vanilla build's launcher has an "Expanded" checkbox on its front page (the
game card). Checked, Play starts the Expanded mod; unchecked, the original game. The choice is saved
with the launcher's settings (`edition_enabled`) and applies when the launcher is set to skip itself.

## How it plays

`src/expanded_edition.cpp`, after the launcher:

| ROM picked | Expanded | Result |
|---|---|---|
| Unmodified (SHA-1 `ec3d126d...`) | off | The original game |
| Unmodified | on | The Expanded ROM is made from it (or reused) and the Expanded build plays it |
| Already this version's Expanded ROM | on | Played as it is |
| Already this version's Expanded ROM | off | A message: turn Expanded on, or pick the unmodified ROM |
| Anything else (an older Expanded ROM, another dump) | on | A message asking for the unmodified ROM; an older Expanded ROM can't be updated |

The Expanded ROM is `<rom name>-expanded.gba` beside the player's ROM, or in `mods/editions/` beside
the executable when that folder can't be written. It is written to a temporary file and renamed, so
a half-written copy is never used, and it is reused while its SHA-1 matches this version
(`ROTK_EDITION_SHA1`). A copy from an older version is replaced. The original ROM is never changed.

The vanilla build then starts the Expanded build (`ROTK_EDITION_EXE`) with `--no-launcher`, the
Expanded ROM, and the launcher's other choices (window, BIOS, filters). Expanded keeps its own save:
`<save>-expanded<ext>` when the session names a save, otherwise the default beside its ROM. It plays
with the mod settings of the launcher that started it (`GBARECOMP_MODS_ROOT`, the vanilla build's
`mods` folder, whose `rotk.pc` manifest lists both ROMs as targets). The vanilla build waits for it
and exits with its exit code.

## Building

The patch is made from the exact Expanded ROM the Expanded build compiles, so the two can't drift.
The Expanded build compiles the mod's PC edition (`make hack-pc` in ../rotk): the same patches with
the mod's RAM in host RAM (0x12001000, reserved in `src/main.cpp`), so PC-only features are not
bound by the GBA's RAM. `prepare` picks `build/rotk_pc.gba` and `build/hack_pc.elf` when they exist:

```powershell
make -C ../rotk hack-pc
python tools/lab.py prepare --target expanded --rom ../rotk/build/rotk_pc.gba --elf ../rotk/build/hack_pc.elf
python tools/lab.py build --target expanded   # also refreshes the launcher's patch, see below
```

`build --target expanded` ends by remaking `.private/expanded/expanded.bps` from the ROM it just built,
reconfiguring `build/host` with that ROM's `ROTK_EDITION_SHA1`, and rebuilding the launcher. Set
`ROTK_EDITION_EXE` once by hand when `build/host` is first configured:

```powershell
cmake -S . -B build/host -DROTK_EDITION_EXE=../host-expanded/ROTK-Recompiled-Expanded.exe
```

## Play does nothing (or "exited with code 1")

Pressing Play with Expanded ticked closes or does nothing, and `Play.ps1` reports exit code 1. The
Expanded exe printed `ROM SHA-1 mismatch: got <old> expected <new>` and quit.

Cause: the Expanded build was re-prepared from a new mod ROM, but the launcher (`build/host`) still
had the old `expanded.bps` and `ROTK_EDITION_SHA1`. The launcher found an old `-expanded.gba` that
matched its old hash and handed it to the new Expanded exe, which only accepts its own ROM. This
happened on 2026-11-02 after `prepare --target expanded` without refreshing the launcher.

Fix: `python tools/lab.py build --target expanded`, which refreshes the launcher. By hand:

```powershell
python tools/make_edition_patch.py     # prints -DROTK_EDITION_SHA1=<sha1>
cmake -S . -B build/host -DROTK_EDITION_SHA1=<sha1>
cmake --build build/host --target ROTK-Recompiled
```

Check: `grep ROTK_EDITION_SHA1 build/host/CMakeCache.txt` must equal the SHA-1 of
`.private/expanded/rotk.gba`. `Play.ps1` checks this before every launch, and warns when they differ.
The game's own reason for quitting is printed just above the PowerShell error.

Related: after a change to the `rotk.pc` manifest or its features, rebuild both `build/host` and
`build/host-expanded`. An exe whose manifest is older fails with "cannot launch with selected mods".

`ROTK_EDITION_EXE` is relative to the vanilla executable or absolute: a release puts both executables
and `expanded.bps` in one folder. The patch is a Flips BPS delta (349 KB for mod 521e3b8);
`RotkBpsPatchCheck <rom> <patch>` applies one and prints the result's SHA-1.

The launcher row is `recomp-ui`'s `GameInfo.edition_label`/`edition_note` and `Settings.edition_enabled`
(`patches/recomp-ui-edition.patch`); the framework passes it through `RunOptions::launcher_edition_*`
(`patches/local-link.patch`).

Checked (2026-10-27): the v0.7 release patch on the unmodified ROM gives the v0.7 ROM (`b8b1db5c...`),
and the build's patch gives the Expanded build's ROM exactly (`b2fcf750...`); any other ROM is refused.

Checked (2026-10-28): the build's patch (mod 521e3b8: Two Towers skills for every Two Towers hero,
the Legacy Skills option) on the unmodified ROM gives the Expanded build's ROM exactly (`ae01d10a...`).
