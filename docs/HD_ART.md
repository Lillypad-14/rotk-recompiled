# HD art

The PC renderer can draw full-resolution, full-color art in place of the GBA's 8x8, 16-color tiles,
like an emulator texture pack. The game still runs and draws its normal tiles; the renderer
recognizes them and draws the HD image instead, at the internal resolution, in the same layer,
priority and blend stack (sprites stay on top, fades still apply). The GBA build is unchanged.

## Built: HD skill icons (2026-10-26, not yet tested in game)

- Renderer (framework, `gba_high_resolution.h/.cpp`): `gba::hd_tiles_set` registers images, each
  tied to the exact 4bpp tile data it replaces. Each frame the PPU finds those tile blocks in the
  text BG tilemaps (whole blocks only, anchored on tiles unique to one image, so shared frame tiles
  cannot mix icons up), and on those pixels samples the image, pre-resampled to the internal scale.
- ROTK (`src/hd_icons.cpp`): launcher feature "HD skill icons" (Display, off by default) with a
  "Render resolution" option (3x/4x/6x). Loads every pack under
  `mods/packages/rotk.pc/<version>/hd/<pack>/manifest.txt` and raises the internal resolution.
- Packs: the romhack's `hacks/patches/ringwraith/icons_hd/` and
  `hacks/patches/skill_expansion/<hero>_icons_hd/` (133 icons), copied into the package by CMake.
  `manifest.txt` lines: `<png> <tiles_w> <tiles_h> <tile data hex>`. PNGs are RGBA with the in-game
  icon's transparent corners; the art stays intact under the alpha.
- Romhack tool (`hacks/tools/skill_icon_art.py`): `slice --hd DIR` saves each icon's full-resolution
  crop, `hd PNG_DIR C_FILE HD_DIR` builds a pack from the icons compiled into C_FILE, and
  `build --hd-out` does it while building. The existing icons' crops were recovered from the art
  sheets by template matching (correlation 0.92-0.99); all 133 tile blocks occur byte for byte in
  the Expanded ROM. `oath_of_the_dead` is cut off at the sheet edge, as its 32x24 icon is.

Next: check both tree pages in game (every icon HD, cursor on top, fades, toggle off restores the
GBA icons), then commit.

## Idea: HD versions of the stock art (for later)

Goal: AI-upscaled or redrawn versions of all static art (level tiles, backgrounds, static objects,
portraits, items, HUD), fed through the same renderer feature.

Sources:

| Art | Extraction | Renderer |
|---|---|---|
| Hero portraits, dialog speaker pictures, cutscene pages | romhack carver `bg_assets` | works (BG tiles) |
| Item/gear icons, level icons, ground drops | carvers `item_gfx`, `level_icons`, `loot_piles` | BG parts work, drops are sprites |
| Level art | new exporter from the cell maps and block tables (format in [WIDESCREEN.md](WIDESCREEN.md)) | needs 32x32 block keying |
| Static objects (stumps, barrels, chests) | sprite carving, not done | needs HD sprites |
| HUD, menu frames | partly carved | works (BG tiles) |

Engine work:

1. Level art keyed by 32x32 block, not 8x8 tile. Blocks share small tiles, and per-tile matching
   would show seams where the AI drew neighbors differently. A ROTK hook would tell the renderer
   "this map cell is block N of this level" from the level data.
2. HD sprites: sprites can flip, scale, rotate and are drawn in pieces.

AI sheets: pictures on padded grids with a plain background, pre-scaled nearest-neighbor, grouped by
kind, each with a map file to slice the result back into a pack. Levels exported as whole assembled
images (context for the AI) plus block sheets for slicing. Animated sprites are a poor fit: an AI
redraws each frame differently, so they wobble.

Constraints: this is the game's own art, so sheets, AI output and packs stay private (`.private/` or
a private pack), never in the public repos. The carver needs the vanilla ROM in `game/`.

Suggested order, each usable alone:

1. Carve the BG pictures and items, build sheets, slicing script. Try one category (dialog
   portraits) through the AI and judge it in game before doing more.
2. Level exporter and block keying.
3. HD sprites, then props and drops.

Estimate: stage 1 a few hours, stage 2 a session or two, stage 3 the largest. The art itself (volume,
checking and fixing AI output, consistency across a map) is likely the bigger effort.
