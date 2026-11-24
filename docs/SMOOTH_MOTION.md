# Smooth motion (launcher feature "Smooth motion (60 fps)", src/smooth_motion.cpp)

On by default. The game ticks once every 2 vblanks (30 Hz), so every second frame repeats. On the
repeated vblank this draws the halfway point between the tick on screen and the next one. It is
presentation only: guest memory, timing and saves are untouched, and the real tick frames are
pixel-identical to the original.

## How the game presents a tick

- `sprite_composeOam(Actor*)` (0x08002484) pushes an actor's pieces through `sprite_pushOam`, directly
  and through `sprite_composeFrame`. `fx_drawParticle` (0x08039324) is the only other pusher.
- Pieces go into one of two OAM shadow buffers (0x03004340, 0x03004B80; `g_OamShadow` 0x03004B40 is
  the one being composed). `sprite_endFrame` sets `frameReady`, and the next VBlank copies that buffer
  to OAM and swaps. Without `frameReady` the VBlank re-copies the buffer on screen.
- The pacing gate starts a tick 2 vblanks after the previous one, so the tick composes during the
  vblank that repeats the displayed one. With the CPU boost it is usually finished before scanline 0.
- World layers BG3/BG2: each tick the game copies the visible 31x21 tiles to VRAM; the scroll
  registers hold only the fine offset (camera & 7). BG1 is parallax, BG0 HUD and text.
- Actor screen position = renderCoords (+0xCC/+0xD0, 16.16) minus `g_CameraPositions[active]`
  (0x030020F8, active player 0x03003F59), except screen-space actors (flag 0x200000) and the overlay
  list.

## What the host does

- Framework hook `g_present_scanline_hook` (patches/smooth-present.patch): `GbaPpu::render_scanline`
  hands the line's BG register image and an OAM copy to the host before drawing.
- Observers (no behaviour change): `sprite_composeOam` entry records the actor and its position,
  `fx_drawParticle` entry does the same for a particle, the existing `sprite_pushOam` hook tags each piece,
  `sprite_endFrame` records the camera of the finished buffer. LR is not used to find the caller,
  since native calls do not keep it.
- On the repeated vblank, when the other buffer finished composing after the displayed one:
  - camera shift = half of the camera change, limited so fine scroll + shift stays in 0..8 (the
    copied tiles cover 8 pixels more than the view), added to BG3/BG2 in gameplay scenes;
  - each tagged piece moves by half of its actor's change, minus the camera shift for world actors;
  - particles: `fx_drawParticle` pieces are tagged with the particle record like an actor (screen
    position = integer renderPosition at +0x1A/+0x1E minus the camera, screen-space when flag 0x200
    at +0x3E is set);
  - BG1 parallax: its next scroll is not stored before the commit, so it moves by its scroll/camera
    ratio learnt from the last two ticks (limited to -2..2, BG1 enabled, gameplay only);
  - changes over 32 pixels (cuts, teleports) are not interpolated.

## Subpixel camera panning (option "Subpixel camera panning", on)

The game keeps positions in 16.16 fixed point but draws on whole pixels, so a camera moving 2.33 px
per tick steps 2, 2, 3 px (and the halfway frames 0, 1, 2 px). With an internal resolution of 2x or
more (HD skill icons' "Render resolution", 6x by default) every gameplay frame is drawn at the exact
camera instead.

- Exact camera: `gfx_updatePlayerCameraScroll` sets the camera to the player's integer position
  (`0x03001A10 + player * 0x30`, Vector2Fp16) minus a view offset, then clamps it. At each tick
  (`sprite_endFrame`) the camera gets the player's fraction on an axis whose offset camera - floor(player)
  is the same as at the previous tick; a clamp (level edge, widescreen), pan or cut changes the offset
  and keeps that axis on whole pixels.
- Presented camera: the tick's exact camera, or the exact halfway point on an in-between frame.
  BG3/BG2 scroll by the whole pixels rounded up and move back right/down by the remaining fraction.
  BG1 parallax does the same at its learnt ratio. The game copies only the visible 31x21 tiles, so the
  fine scroll plus the shift has to stay in 0..8 (see "Copied-tile edges" below). Each tagged piece moves from floor(position) - camera to its exact (or halfway) position minus
  the presented camera: whole pixels in OAM, the rest as a fraction. Untagged pieces and BG0 (HUD) stay.
- Framework (`patches/subpixel-pan.patch`): `gba::g_subpixel_shift` holds per-BG and per-OBJ
  fractions (1/256 px, moving content right/down), set by the present hook at scanline 0. On such lines
  the high-resolution frame keeps each layer's pixel per native column (OBJ: the two frontmost, with
  their OAM index from the compositor key) for this row and the one above, and composites every output
  subpixel from the column/row that covers it after the shift. It only composites where a layer's
  source changes and repeats the subpixel otherwise. Lines with affine or HD tile layers keep the
  regular resolve. Guest state and the native framebuffer are unaffected apart from the whole-pixel
  part (screenshots and `--dump-png` show the world up to 1 px ahead).

Copied-tile edges (`patches/tilemap-window.patch`): moving left/up while the fine scroll is near 0,
or right/down while it is near 7, the halfway camera needs a column or row just outside the copied
tiles. Clamping it there dropped up to 1.5 px every 8 px of travel (a 1.0/2.0 or 0/3 px step instead
of 1.5/1.5), a visible hitch every few frames while walking; the non-subpixel path had the same clamp.

- Widescreen on: `src/widescreen.cpp` marks the copied 31x21 tiles of BG2/BG3 as the valid tilemap
  window (`gba::g_ws_tilemap_valid`); the expanded renderer takes map entries outside it from the
  same cell-map provider as the margins, so the world layers can scroll any distance. No clamp.
- 240 px view: the shift stays inside the copied tiles plus what a fraction adds without new tiles:
  up to a pixel to the left/top (content moved right/down by up to 255/256), a pixel to the right (the
  whole pixels rounded down and the content moved left, `g_subpixel_shift.bg` x is signed), and one
  row at the bottom (the last line scrolls one row less and repeats the row above). Larger halfway
  shifts are clamped to that range.

Checks (2026-11-24, stock, widescreen, 6x, the walk route of the 120 Hz test):

- Walking down: the world moves a steady 7 output px (1.17 GBA px) every frame; with the option off
  it moves 0, 6 or 12. Frames show no seams, the HUD stays put.
- `[rotk] smooth motion: ... 5276 subpixel (1398 at a fractional camera)` on 9,000 frames.
- Cost (frames 7600-9000, `GBARECOMP_FRAME_PHASE`): guest time per frame 6.2 ms mean (3.8 ms off),
  frame intervals unchanged (16.74 ms mean, 17.2 ms p95).

Copied-tile edges, checked on a 16,000-frame walk in all eight directions with dialogue skips
(`.private` input, frames 7600-16200): halfway frames whose presented camera is more than 0.1 px from
the exact halfway point went from 83 of 3,098 to 1 of 727 (widescreen) and 2 of 744 (240 px), the
remaining ones being dialogue openings. Per-frame camera steps no longer alternate except where
walking starts or stops. High-resolution dumps at the formerly clamped frames show no seams at the
view edges or the margin boundary.

Debug: `ROTK_PC_TRACE_SUBPIXEL=1` logs each camera change (integer, exact and player position);
`ROTK_PC_TRACE_PRESENT=1` logs every presented frame's camera (presented, exact, next, fp16);
`GBARECOMP_FRAMEDUMP_HIRES=1` makes `GBARECOMP_FRAMEDUMP_DIR` write the internal-resolution frame.

## Checks (2026-10-26)

- Off: stock first-map frame hash unchanged (`824b55b9...`).
- On: same game state (saved state differs only in one byte that also differs between plain runs),
  real tick frames identical, about 40% of frames interpolated on the routes.
- A short walk (stock, frames 5021..5024): the repeated frames 5022 and 5024 are new in-between frames,
  the player sprite moves.
- Camera scrolling: the headless routes never scroll (the first map's crebain tutorial box keeps
  blocking movement); the user tested walking in-game and it works.

## Known gaps

- Particles and parallax were added after the first in-game test and have not been seen in-game yet.
- Without the CPU boost the next tick is often not ready in time, and those frames repeat as before.

Options: "Smooth particles", "Smooth parallax background" and "Subpixel camera panning" (all on).
Env overrides: `ROTK_PC_SMOOTH` = 0 | 1 for the feature, `ROTK_PC_SMOOTH_PARTICLES`,
`ROTK_PC_SMOOTH_PARALLAX` and `ROTK_PC_SMOOTH_SUBPIXEL` = 0 | 1 for the options.
