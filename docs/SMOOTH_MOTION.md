# Smooth motion (launcher feature "Smooth motion (120 fps)", src/smooth_motion.cpp)

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

## Checks (2026-10-26)

- Off: stock first-map frame hash unchanged (`824b55b9...`).
- On: same game state (saved state differs only in one byte that also differs between plain runs),
  real tick frames identical, about 40% of frames interpolated on the routes.
- A short walk (stock, frames 5021..5024): the repeated frames 5022 and 5024 are new in-between frames,
  the player sprite moves.
- Camera scrolling: the headless routes never scroll (the first map's crebain tutorial box keeps
  blocking movement); the user tested walking in-game and it works.

## 120 Hz second subframe (2026-11-24)

The presentation loop now shows two images per guest vblank, paced at 2 x 59.7275 Hz: the primary
frame at the vblank boundary and a second subframe half a vblank later. `smoothMotion120()` opts in
through `RunOptions::present_extra_frame`; `ROTK_PC_SMOOTH_HZ=60` keeps the old 60 fps cadence.

- On a repeated vblank the extra frame is the 3/4 point of the tick change (the primary is the 1/2
  point). On a real tick it is a 1/4 extrapolation of the previous tick's motion, so a constant
  velocity advances in equal 1/4 steps.
- The runtime re-renders the frame by calling `GbaPpu::render_scanline()` for every line with
  `g_present_extra_render` set. The hook therefore sees the same guest state, same tile camera and
  same OAM as the primary frame, and applies the extra offsets per line. High-resolution output is
  rebuilt at the same scale.
- Cost is one extra full-frame render per guest frame, so 6x internal resolution roughly doubles
  presenter work. Fast-forward and disabled smoothing keep a single present.
- `docs/` checks use `smooth motion: ... extra 120 Hz frames (moved)` to show the second subframe ran
  and that it actually displaced something.

## Known gaps

- Particles and parallax were added after the first in-game test and have not been seen in-game yet.
- Without the CPU boost the next tick is often not ready in time, and those frames repeat as before.

Options: "Smooth particles" and "Smooth parallax background" (both on). Env overrides:
`ROTK_PC_SMOOTH` = 0 | 1 for the feature, `ROTK_PC_SMOOTH_PARTICLES` and `ROTK_PC_SMOOTH_PARALLAX`
= 0 | 1 for the options.
