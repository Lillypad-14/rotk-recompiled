# Minimap

Launcher feature "Minimap" (`minimap`, Display, off by default), `src/minimap.cpp`. Option: show
enemies (on).

The top right of the gameplay screen shows the level, at one native pixel per 32x32 block (more blocks
per pixel past 72 blocks a side), with this console's view as a white box, this console's player in
cyan, the other player in yellow and enemies in red. It is drawn into each shown frame through
`present_decorate`, like the health bars (docs/COMBAT_INFO.md), from the game's memory only. In co-op
each screen has its own.

The level comes from the collision map that `terrain_getCollisionAt` (0x08034770) reads: the pointer
at ROM 0x080347D0 leads to one u16 per 32x32 block (block id in the low 12 bits), the pointer at
0x080347D4 to 16 collision bytes per block id, one per 8x8 tile. A block is light when most of its
tiles have collision 0, mid grey when some do, dark when none do. The map is rebuilt when the block
map or the world size (`g_WorldWidthTiles`, `g_WorldHeightTiles`) changes.

`ROTK_PC_MINIMAP=1` turns it on for tests, `ROTK_PC_MINIMAP_ENEMIES=0` hides the enemies.

Checked on the 15:25 co-op recording (2026-10-27, linked frame 3,640): both screens show the level's
corridors, their own view box and player, the partner and the fight's enemies.
