# Widescreen

Goal: a wider gameplay view as a launcher feature (off by default), using GBARecomp's expanded-view
renderer (`gba_ppu.h` margins and per-game providers). MinishCapRecomp's adaptive view
(`src/minish_extended_view.cpp` in mstan/MinishCapRecomp) is the reference for the pieces a GBA game
needs: culling constants, map margins from the game's full map, HUD backgrounds and HUD sprites.

## What ROTK does (measured on the vanilla first map, frame 7,500)

- Mode 0, all four backgrounds 32x32 tiles (256 px rings).
  - BG0: priority 0, charblock 3, screenblock 29, no scroll. HUD/text layer (to confirm).
  - BG1: priority 3, screenblock 28. BG2: priority 1, screenblock 30. BG3: priority 2, screenblock 31.
- World size in 8 px tiles: `g_WorldWidthTiles` (0x03005F90), `g_WorldHeightTiles` (0x03005F88).
  First map: 144 x 160 tiles.
- Camera: `g_CameraPositions[2]` (0x030020F8, per player, top-left world pixel).
  `gfx_updatePlayerCameraScroll` (0x0803E838) centres the player (minus 120, 80) and clamps to
  `[0, worldW - 240] x [0, worldH - 160]` (immediates 0xF0 and 0xA0). A second camera at 0x03002140
  drives a parallax layer. The low 3 bits of both feed the hardware fine scroll.
- Levels are metatiles: per layer a cell map of u16 block numbers, one per 4x4 tiles (32 px), and a
  block table of 16 tilemap entries per block. The level loaders at 0x0803EF7C and 0x0803F0BC
  decompress them (0x08019D54) from a level header (width and height in cells at bytes 0 and 2).
  First map: 36 x 40 cells, cell maps 0xB40 bytes.
  - Globals (unnamed yet): cell-map pointers 0x03005F5C, 0x03005868, 0x030060C0, 0x03005F8C;
    block tables 0x030060D0 (0x02032D60 on the first map), with sizes at 0x030060CC (width tiles),
    0x030060D4 (height tiles), 0x030060D8 / 0x03005FC8 (cells).
- The screenblocks are not rings. ARM code copied from ROM 0x0800D590 to IWRAM 0x03005FD0 DMAs the
  visible 31x21 tiles every frame from two 36-wide IWRAM staging buffers (0x03005880 for BG3,
  0x030062B0 for BG2), filled by the builder just before it in ROM (0x0800D3xx) from the cell maps:
  - BG3: cell map `*(u32 *)0x03005FB4`, block table `*(u32 *)0x03005FBC`.
  - BG2: cell map `*(u32 *)0x0300586C`, block table `*(u32 *)0x03005FC0`.
  - `entry = u16 at blocks + cell * 32 + (ty & 3) * 8 + (tx & 3) * 2`, with
    `cell = u16 at cellMap + ((ty >> 2) * cellsPerRow + (tx >> 2)) * 2` and
    `cellsPerRow = *(u16 *)0x03005878`. Verified 600/600 tiles on both layers.
  - VRAM column 0 is world tile `camera.x >> 3`, row 0 is `camera.y >> 3`.
- BG1 is a repeating 8x8-tile pattern filling the 256x256 screenblock: hardware wrap is correct.
- On-screen test: `actor_updateVisibility` (0x08000C38): camera box plus 40 (or 100) px. Actors
  outside neither tick nor draw.

## Done (2026-10-26)

`src/widescreen.cpp`, launcher feature "Widescreen" (off by default; 16:10, 16:9, 2:1, 21:9).

- Margins: `g_ws_tilemap_provider` returns BG2/BG3 world tiles from the cell maps (transparent past
  the level edge), keeps BG1 wrapped, leaves BG0 out of the margins. Non-gameplay scenes are
  pillarboxed. The camera is latched when the game DMAs BG3's rows, so margins and centre share it.
- Camera: a hook on `_call_via_sl` (0x0804E0B8) when called from gfx_updatePlayerCameraScroll
  (LR 0x0803EC49) re-clamps the active camera to [left margin, worldWidth - 240 - right margin].
- Culling: host `actor_updateVisibility` with the box widened by the margins. With no margins it
  reproduces the original frames (`ROTK_PC_TEST_VISIBILITY=1`, vanilla first-map hash unchanged).
- Checks: seamless margins on the first map, the 400-enemy crowd draws and moves in the margins,
  widescreen off keeps the vanilla and Expanded hashes.

- HUD: on frames with the standard gameplay HUD (BG0 row 18 starts with the red bar tile 0x28 at
  column 3 and the blue bar tile 0x30 at column 19, row 19 has the panel filler 0x0005, rows 4-15 are
  empty), `g_ws_bg_x_provider` anchors BG0 columns 0-10 to the left edge and 19-29 to the right edge,
  keeps 11-18 (ornament, or the Expanded mod's meter) centred and repeats the filler along the panel.
  HUD sprites (pieces pushed between `actor_tickAllInList`'s entry and `fx_buildRenderQueues`, i.e.
  the fixed render queue) move to their side with `g_ws_obj_attr_x_provider`. Dialogue boxes, menus
  and cutscenes keep the native layout. `sprite_composeOam` is not hooked: in the Expanded build its
  entry is a trampoline and a hook there changed the frames even with widescreen off.

## Next

1. Other culling: enemy spawners, projectiles and effects that check the camera themselves.
2. Co-op (two cameras) and cutscene camera pans.
3. Adaptive width (follow the window aspect) as an extra choice.
4. In-game dialogue during gameplay: check it keeps its layout (the HUD rule should leave it alone).
