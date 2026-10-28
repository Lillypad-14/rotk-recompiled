# PC-port milestones

## 1. Repeatable stock-game route

Build the pinned runtime, regenerated cartridge code, and generated BIOS. Verify cold boot, language selection, title, character selection, and the first playable map.
Create an input replay that demonstrates movement, attacking, receiving damage, one skill, pause/resume, room transition, and EEPROM save/reload.
Retain screenshots and coverage per checkpoint. Compare the same route in mGBA; matching the framework's own interpreter alone does not prove its hardware model is correct.

## 2. Execution coverage

Investigate each missing IWRAM/ROM entry and record whether it is an indirect call, an interior resume, or modified RAM code.
Fix discovery or runtime support at the appropriate source layer. Review metadata changes instead of copying miss proposals blindly or editing generated C++.
Run strict native checkpoints separately. A cached runtime recompilation is useful progress but remains distinct from ahead-of-time coverage.

## 3. Expanded compatibility

Add the tested Expanded ROM as a separately verified target. Import symbols from its linked ELF, including code outside the original cartridge's 16 MiB.
Validate the mod's startup hooks, extra RAM reservations, engine tables, saves, characters, Gauntlet scenes, and custom effect data.
Keep stock and Expanded configurations and saves separate.

## 4. Replace one engine service

Use the original game as the behavior reference. Start with a contained service and keep a faithful build available for comparison.
Potential experiments include a host-rendered HUD or an independently validated native graphics upload path. Measure the result before expanding the scope.
Replacing the actor allocator or actor loop needs guest-pointer compatibility, collision/targeting/lifetime checks, and a plan for fixed-size actor tables. More host memory alone cannot extend those structures.
Larger sprite/effect capacity needs a renderer and submission path that can represent it; changing the CPU's execution backend does not change GBA OAM or tile memory.

## 5. PC enhancements

Once a playable route and service replacement are qualified, investigate larger actor pools, heavier spell effects, wider views, more flexible interfaces, and host storage.
Keep changes to gameplay timing and simulation explicit, with reproducible fight scenarios and saved-state checks. These enhancements are not claimed by the initial boot experiment.

## 6. Visual upgrades beyond the GBA (ideas, 2026-10-26)

Each would be a launcher option in the rotk.pc package. In suggested order after the HD audio mixer:

1. 60 fps motion: the game updates about every 2.2 vblanks; draw in-between frames by interpolating
   actor and camera positions on the host. Gameplay timing stays unchanged.
2. More effects on screen: raise the effect and particle pool caps (and the mod's effect cap) now that
   actor and sprite limits are gone.
3. Smooth rotation and scaling: the framework's higher internal resolution for affine graphics.
4. Real transparency: per-sprite alpha in the host renderer instead of the GBA's single blend mode
   (soft shadows, auras, translucent wraiths without flicker).
5. More colours: per-level colour grading or lighting tints beyond 15-bit, 16-colour palettes.
6. Overlay layers: weather, fog or vignettes through the framework's screen overlay, without using the
   game's four background layers.
7. Screen filters: CRT, LCD grid or smooth upscaling.
8. Sharper HUD and text drawn by the host at full resolution.
9. HD art: full-resolution, full-color art in place of GBA tiles. HD skill icons are built;
   AI-upscaled or redrawn stock art (levels, portraits, items, HUD) is planned in [HD_ART.md](HD_ART.md).

Built so far: 60 fps motion ([SMOOTH_MOTION.md](SMOOTH_MOTION.md)), real transparency, and HD skill icons,
all of which also run in split-screen co-op.

## 7. Next ideas (2026-10-27)

Built the same day, each a launcher option off by default unless noted: health bars and damage
numbers ([COMBAT_INFO.md](COMBAT_INFO.md)), a minimap ([MINIMAP.md](MINIMAP.md)), controller rumble,
color vision correction, instrument sample packs (the code and the dump tool, no packs yet;
[AUDIO.md](AUDIO.md)), save states and 20 s of rewind (on), the F12 bug report and a co-op replay
viewer ([BUG_REPORTS.md](BUG_REPORTS.md)), and `tools/regress.py`. Key remapping was already in the
launcher. Still open below: online co-op, performance, HD stock art, a larger HUD and text,
hold-to-attack (the input hook can only add presses), and the other platforms.

Bigger directions, in suggested order:

1. Online co-op: the game's own link co-op between two PCs, over the framework's netplay layer. The
   split-screen work (page 2 and options sync, the desync fixes, recordings and `tools/check_coop.py`)
   carries over. The open problems are network latency and rollback, not game logic.
2. Performance: two consoles in co-op and busy fights still go over the frame budget at times. Known
   costs are the exception thrown at every co-op chunk end (about 15%), scanline rendering (27% in
   co-op) and the per-instruction tick (8%). See [SPLIT_SCREEN.md](SPLIT_SCREEN.md) and
   [PERFORMANCE.md](PERFORMANCE.md).
3. HD stock art and a sharper host-drawn HUD and text (section 6, items 8 and 9).

Player-facing, each a launcher option drawn or played by the host, so the simulation and co-op are
unchanged:

- Enemy health bars and damage numbers, through the host overlay.
- A minimap of the level's tile map with both players' positions.
- Controller rumble on hits, critical hits and level-ups.
- Quick save and rewind on the framework's save states, including the Expanded mod's state.
- Accessibility: colorblind palettes, a larger HUD and text, hold-to-attack, remappable controls.
- Better instrument samples for the music. The HD audio mixer reads each voice's instrument sample
  from the ROM, so a sample pack keyed by sample address could replace them, like the HD icon packs.
  No replacement samples exist yet: each would be sourced (a permissively licensed SoundFont, or an
  upscaled original), matched by ear and tuned to the original's pitch and loop points.

Co-op:

- A bug report button that bundles the last session's recording and both starting saves, which replay
  the report exactly.
- A replay viewer for recorded sessions.

Platforms: Steam Deck and Linux builds, then Android (the framework has touch controls).

Project health: replay every co-op recording and gameplay route against each new build and flag
link drops, checksum errors, random-stream splits and slow frames. `tools/check_coop.py` already does
this for one recording.
