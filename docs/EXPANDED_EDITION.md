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
python tools/lab.py build --target expanded
python tools/make_edition_patch.py     # .private/expanded/expanded.bps, prints the CMake settings
cmake -S . -B build/host -DROTK_EDITION_PATCH=<bps> -DROTK_EDITION_SHA1=<sha1> -DROTK_EDITION_EXE=<expanded exe>
python tools/lab.py build --target stock
```

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
