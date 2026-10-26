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
  visible 31x21 tiles every frame from two 36-wide IWRAM staging buffers (0x03005890 for BG3,
  0x030062B0 for BG2), filled by the builder just before it in ROM (0x0800D3CC) from the cell maps:
  - BG3: cell map `*(u32 *)0x03005FB4`, block table `*(u32 *)0x03005FBC`.
  - BG2: cell map `*(u32 *)0x0300586C`, block table `*(u32 *)0x03005FC0`.
  - `entry = u16 at blocks + cell * 32 + (ty & 3) * 8 + (tx & 3) * 2`, with
    `cell = u16 at cellMap + ((ty >> 2) * cellsPerRow + (tx >> 2)) * 2` and
    `cellsPerRow = *(u16 *)0x03005878`. Verified 600/600 tiles on both layers.
  - Both the builder and the DMA take their position from the tile cameras at 0x03005880 (Vector2Int
    per player, read as `0x03005880 + player * 8`), not from `g_CameraPositions`. VRAM column 0 is
    world tile `tileCamera.x >> 3`, row 0 is `tileCamera.y >> 3`.
- `gfx_updatePlayerCameraScroll` stores the clamped camera in `g_CameraPositions`, the tile cameras
  and a third copy (0x03005FA0), then, for the active player, the fine scroll of BG3 (x 0x03005FB8,
  y 0x03005F50) and BG2 (x 0x0300211E, y 0x03002120), and finally calls the commit through
  `_call_via_sl`. Disassembly: 0x0803EBD0-0x0803EC44.
- BG1 is a parallax backdrop that wraps on its own: a repeating 8x8-tile pattern on the first map, an
  animated-water pattern moving at about a quarter of the camera speed on the river levels.
- On-screen test: `actor_updateVisibility` (0x08000C38): camera box plus 40 (or 100) px. Actors
  outside neither tick nor draw.

## Done (2026-10-26)

`src/widescreen.cpp`, launcher feature "Widescreen" (off by default; 16:10, 16:9, 2:1, 21:9).

- Margins: `g_ws_tilemap_provider` returns BG2/BG3 world tiles from the cell maps (transparent past
  the level edge), keeps BG1 wrapped, leaves BG0 out of the margins. Gameplay and the in-game
  dialogue overlay (scene 0x11) keep the wide view; other scenes are pillarboxed. The tile camera is
  latched when the game DMAs BG3's rows, so the margins continue exactly the tiles in VRAM. With
  smooth motion the tile follows the shifted scroll of the in-between frame (`smoothBgShift`).
- Camera: a hook on `_call_via_sl` (0x0804E0B8) when called from gfx_updatePlayerCameraScroll
  (LR 0x0803EC49) re-clamps the active camera to [left margin, worldWidth - 240 - right margin] in
  all three camera copies and in both world layers' fine x scroll.
- `ROTK_PC_TRACE_MARGINS=1` logs frames where the cell-map lookup does not reproduce the visible
  30x20 tiles of BG2/BG3 in VRAM. It stays silent during gameplay except on black fade-in frames
  before a level's first tile copy.
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

## Polish pass (2026-10-26)

Found by replaying a recorded play session (diagnostic replay, every frame dumped) on the build it
was recorded with.

- Map edges: the [bugfix pass](BUGFIX_PASS.md) took 0x03005880 for BG3 staging and removed the
  clamp's write there. It is the tile camera (staging starts at 0x03005890), so at the left and right
  edges the game built VRAM for the unclamped camera while BG3 scrolled by the clamped one and BG2
  did not move: centre layers misaligned by up to 7 px and the margins 1-2 tiles off. Fixed as above;
  `RotkWidescreenEdgeCheck` now checks every copy, both fine scrolls, the other player and staging.
- Pixelated sides while walking (smooth motion on): the renderer picks the pixel within a margin tile
  with the half-step scroll, the tile came from the unshifted scroll, so each margin tile showed a
  1-4 px strip of its neighbour. Visible as streaks along cliff and water edges.
- In-game dialogue switched the view to black bars and back.

Verified on the recorded session: the river-level segment replays bit-identically with the fixes and
its margins show no streaks; the first-map start at the left edge (driven by the session's inputs)
has seamless margins; the edge-walk replay completes with the margin trace silent.

## Next

1. Other culling: enemy spawners, projectiles and effects that check the camera themselves.
2. Co-op (two cameras) and cutscene camera pans.
3. Adaptive width (follow the window aspect) as an extra choice.
