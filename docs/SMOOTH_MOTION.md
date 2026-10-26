# Smooth motion (launcher feature "Smooth motion (60 fps)", src/smooth_motion.cpp)

Off by default. The game ticks once every 2 vblanks (30 Hz), so every second frame repeats. On the
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
  `fx_drawParticle` entry ends that actor, the existing `sprite_pushOam` hook tags each piece,
  `sprite_endFrame` records the camera of the finished buffer. LR is not used to find the caller,
  since native calls do not keep it.
- On the repeated vblank, when the other buffer finished composing after the displayed one:
  - camera shift = half of the camera change, limited so fine scroll + shift stays in 0..8 (the
    copied tiles cover 8 pixels more than the view), added to BG3/BG2 in gameplay scenes;
  - each tagged piece moves by half of its actor's change, minus the camera shift for world actors;
  - changes over 32 pixels (cuts, teleports) are not interpolated.

## Checks (2026-10-26)

- Off: stock first-map frame hash unchanged (`824b55b9...`).
- On: same game state (saved state differs only in one byte that also differs between plain runs),
  real tick frames identical, about 40% of frames interpolated on the routes.
- A short walk (stock, frames 5021..5024): the repeated frames 5022 and 5024 are new in-between frames,
  the player sprite moves.
- Not seen yet: camera scrolling. Both test routes stay on the first map, where the crebain tutorial
  box keeps blocking movement, so the camera never moves headless. Needs an in-game test.

## Known gaps

- Particles (`fx_drawParticle`) and BG1 parallax still move at 30 fps.
- Widescreen draws its margins from the cell maps at the tick's camera; the shifted world layers may
  disagree with them by the half step. Test with widescreen off first.
- Without the CPU boost the next tick is often not ready in time, and those frames repeat as before.

Env: `ROTK_PC_SMOOTH` = 0 | 1 overrides the package feature.
