# Host palettes (src/host_palettes.cpp, patches/host-palettes.patch)

The GBA has 16 OBJ palette banks and an OAM entry names one in 4 bits. ROTK keeps banks 0..8 fixed
(heroes, then `ObjPalettes[2..8]`: bank 2 holds the heroes' held weapons) and shares 9..15 between
everything else (enemy palettes, fx_kit's custom palettes). The ported Two Towers skills bring about
25 palettes of their own, and Gandalf alone can have 6 on screen at once (Zap/Shield, Sunburst,
Fireworks, Thunderstrike, the eagle, the trail lights). With an enemy palette that fills every shared
bank, and the effect that comes last has nowhere to go.

On PC the renderer is ours, so a sprite piece can carry its own 16 colors:

- Renderer (`patches/host-palettes.patch`): `gba::g_obj_palette_provider(index)` gives an object (OAM
  index, 128 and up for extra pieces) 16 RGB555 colors to draw in instead of its 4bpp bank, in every
  OBJ path of `gba_ppu.cpp`. 8bpp objects keep the 256-color palette.
- Host: observers on `sprite_composeOam` (the actor) and `fx_drawParticle` (no actor) name each
  piece's owner, and the `sprite_pushOam` hook records, per shadow buffer, the piece's colors when
  the owner's current animation (`Actor+0x24`) has its palette (`SpriteAnimation+8`) in host RAM
  (0x12000000..0x16000000). Only sprites the host converted have one there, so stock and mod sprites
  are never touched. The provider answers from the buffer the VBlank handler last copied to OAM.
- Mod: `PcHostInfo.features` bit 0 (`PC_HOST_FEATURE_HOST_PALETTES`, info version 6). With it,
  `legacyFx_setSprite` binds the animation without loading its colors anywhere and the allies claim
  no bank, so the shared banks and bank 2 stay as the stock game has them. Without it (a GBA, an
  older host) legacy effects claim shared banks (`hackLib_fxUseColors`) and, with none free, borrow
  bank 2 one palette at a time until one frees up (`legacyFx_frameTick`).

On when the Two Towers data is loaded. Single console only: split-screen co-op keeps one state (the
legacy skills are single player).
