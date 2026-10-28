# Health bars and damage numbers

Launcher feature "Health bars and damage numbers" (`combat-info`, Display, off by default),
`src/combat_info.cpp`. Options: enemy health bars, bars on unhurt enemies (off), damage numbers.

Each shown frame, the host walks `g_ActorListMain` and draws into the frame through the framework's
`RunOptions::present_decorate` (`patches/local-link.patch`): a copy of the frame at the internal
resolution, after the game's frame and before it is presented. Geometry is in native pixels scaled
to the internal resolution, so bars and the 3x5 digits look like GBA graphics at any scale. The
game's memory is only read: savestates, frame hashes, the normal game's frame dumps and co-op are
unchanged. In co-op each console's screen is drawn from its own memory (co-op frame dumps include
them).

An enemy is a non-player actor (no `ownerChar`) with an enemy unit type (below
`LOOT_UNIT_TYPE_CONTAINER`) and health, except loot piles, chests and gem rain, which reuse
`currentHp`. Mission-placed enemies are actor type 25, spawned ones 26 and others exist, so the type
is not tested. The game keeps no maximum health on the actor (it is scaled by difficulty when an
enemy spawns), so the bar's maximum is the highest health seen since the enemy appeared. A damage
number is a drop in health between shown frames, including the killing blow. Nothing draws while a
menu is over the world (`GAME_FLAG_OVERLAY`, `GAME_FLAG_WORLD_PARKED`) or outside gameplay.

`ROTK_PC_COMBAT_INFO=1` turns the feature on for tests, `ROTK_PC_HEALTH_BARS`,
`ROTK_PC_HEALTH_BARS_ALWAYS` and `ROTK_PC_DAMAGE_NUMBERS` = 0 | 1 set the options.

Checked on the 15:25 co-op recording (2026-10-27): during the fight at linked frames 3,540 to 3,700
bars show over the hurt enemies and damage numbers (7, 8, 10) rise from them on console 1's screen.
