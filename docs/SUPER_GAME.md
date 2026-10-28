# The War of the Ring: Two Towers + ROTK (plan)

A PC-only "super game": The Two Towers GBA and Return of the King in one window, with a hero that
carries over. GBA compatibility does not matter here; the standalone Expanded romhack remains the
version for original hardware.

## What the player sees

1. **ROTK's main menu, almost unchanged, plus one button: "The Two Towers".** It switches
   seamlessly into The Two Towers' own main menu (same window, no relaunch). Quitting Two Towers
   returns to ROTK's menu.
2. **Import a Two Towers hero at any point.** ROTK's save screen gets "Import from The Two Towers":
   the PC port reads the Two Towers save and ROTK builds a new slot from it (hero, level, stats,
   gear, legacy skills). The Two Towers save is left untouched; importing again makes a new copy.
   One way only: nothing goes back to Two Towers.
3. **Skill page 3: Legacy skills**, to the right of Expanded's page 2. The hero's Two Towers
   actives and passives ported 1:1 with the Expanded skill framework. Skills learned in Two Towers
   arrive at their level; the rest can be bought with ROTK skill points.
4. **Optional: ROTK hero sprites in Two Towers** (a toggle), so the hero looks the same in both.

## Phases

| # | Phase | Notes |
|---|---|---|
| 0 | **Map Two Towers** with ROTK's names | `ttt_match.py`, `ttt_save.py` (player and save mapped, below) |
| 1 | **Two Towers on PC** (recompile, launcher, the PC extras) | biggest risk: no decomp, expect crash-fix iterations |
| 2 | **Menu button + seamless switch** | mod adds the button; the PC port swaps carts (both games in one exe) |
| 3 | **Character import** | ROTK save format is known; needs the TTT player/save layout |
| 4 | **Legacy skills page 3**, hero by hero | per-hero kit work, like an Expanded kit |
| 5 | **ROTK sprites in Two Towers** (toggle) | shared sprite engine, mostly data |
| later | Two Towers split-screen co-op | TTT has the same link system (`g_MpLinkState`) |

## Phase 0 findings (2026-10-28)

Two Towers ROM: "TWO TOWERS", code ALPE, 16 MB, SHA-1 `82b3b4afb0ed0d3fca646aa266e0bfc5bffd49d9`,
EEPROM save (`EEPROM_V122`), same Griptonite engine as ROTK.

The Two Towers tools live with the decompilation, in the mod repository's [`two-towers/`](https://github.com/Lillypad-14/ROTK-Expanded-experimental/tree/main/two-towers)
(`game/two-towers/` here once the `game` submodule is past commit 820cbce). Run them from that
repository's root with `rotk.gba` and `ttt.gba` beside them.

`python two-towers/tools/ttt_match.py --ttt ttt.gba` (about 8 minutes) writes `build/ttt/symbols.json`:

- **487 of 1054** named ROTK functions found in Two Towers: 210 by masked bytes, 233 aligned by
  instruction shape between matched neighbours (both games keep the same link order), 44 through
  calls. Coverage now reaches player, save, skill-tree and save-menu code.
- **138 RAM globals**, voted from literal-pool loads at the same place in aligned code:
  `g_PlayerGlobals` 0x03003640 (93/99 votes), `g_PlayerHuds` 0x030038B0, `g_SaveSlots` 0x03004520,
  `g_MpPeerSaveSlot` 0x030044A0, `g_SaveGlobals` 0x03004700, `g_SceneCurrent` 0x03003A50,
  `g_GameFlags` 0x030039A8, `g_PlayerKeysPressed` 0x030038E0, `g_MpLinkState` 0x030062E0.

### Two Towers Player (stride 0x128, ROTK 0x1AC)

Field order differs completely from ROTK's. From paired functions (player_addHp, player_awardXp,
player_addCorruption, ...) and the save serializer:

| TTT | ROTK | field |
|---|---|---|
| +0x04 | +0x178 | experience (u32) |
| +0x08 | +0x17C | currentHp |
| +0x0A | +0x17E | currentSpirit |
| +0x0C | +0x184 | gems |
| +0x0E | +0x186 | killCount |
| +0x14 | +0x19B | byte capped at 100 (ROTK corruption's slot in player_addCorruption) |
| +0x16 | +0x19C | level |
| +0x17 | +0x19D | attributePoints |
| +0x18 | +0x19E | skillPoints |
| +0x1A | +0x19F | heroId |
| +0x25..0x2F | +0x158.. | 11 skill levels |
| +0x6C / +0x6E | +0xD0 / +0xD2 | stats max HP / max spirit (TTT's stat order differs) |
| +0xBC | +0x3C | inventory (0x3C bytes; ROTK 0x40) |
| +0xFA..0xFE | baseStats | 5 base attributes (bytes) |
| +0x104 | +0x00 | ownerActor |

`activePlayerCount`/`activePlayerIndex` sit at +0x250/+0x251.

### Two Towers save

`save_serializePlayer` 0x0803532C, `save_deserializePlayer` 0x080350F0, `save_readSlot`
0x080356B8, `save_writeSlot` 0x080356F4. EEPROM 512 bytes, header "LORGBA03" (4 blocks), then
**4 slots of 0x78 bytes** at blocks 4/19/34/49 (table at ROM 0x0805765C). Each slot ends in a
negated halfword checksum (the slot sums to 0). The `.sav` stores each 8-byte block byte-reversed,
as ROTK's does. `python two-towers/tools/ttt_save.py "<TTT .sav>"` decodes it; the slot fields are listed there.

Confirmed against two real saves and the character screen (Gandalf, 2026-10-28): hero 3 is
Gandalf; base attributes are stored strength, accuracy, health, defense, courage (ROTK's order);
gems and kills match. From TTT's player_awardXp (0x0800D8FC): the saved XP is **XP since the last
level-up** (the screen's "Experience"), against a per-level table at ROM 0x08050348 (100, 300,
600, 1000, ...); a level-up gives 5 attribute points and 1 skill point, and every 500 kills gives
1 skill point. There is **one skill point pool** (+0x18) shown on both pages; slot byte 0x47
(Player +0x100) is a flags byte (bit 1: level-up notice). Import needs lifetime XP, which
`ttt_save.py` rebuilds as `totalExperience`.

### Two Towers text, heroes and items

- **Text:** the same Huffman packages as ROTK (table at 0x0832EBA4, `text_setLanguage`
  0x0803E9F8); rotkit's `textdb` decodes them. 1610 English strings, dumped to
  a private `text_en.tsv` (not in either repository).
- **Heroes:** 0 Frodo, 1 Legolas, 2 Aragorn, 3 Gandalf, 4 Eowyn, 5 Gimli (text 18..23). The
  level-up code stops at level 50.
- **Skills:** 11 per hero; the save's `skillLevels` follow each hero's description block (text
  348 Frodo, 359 Legolas, 370 Aragorn, 381 Gandalf, 392 Gimli, 403 Eowyn), passives first, then
  actives; "(Level N)" in the text is the character level required. Verified on Gandalf only:
  his starting Magical Healing is byte 4, and three saves add up point for point.
- **Not yet verified in the game** (from the zephyrmaster GameFAQs guide, saved in Desktop/lotr;
  the game wins where they differ): Gimli is multiplayer-only; the whetstone is an equipment slot
  (the save has 7 worn item slots, so it is probably a level byte); max level 52 (the code says 50).
- **Items:** 4 bytes, `prefix, prefix2, base, suffix` (0xFF = none). Three tables of 16-byte
  records (`u16 nameId, s16 gem value, 5 x (u8 stat, s8 value)`, stat 0x40 = none): 152 prefixes
  at 0x080504F0, 127 bases at 0x08050E70 (every item type in one table, the last 21 uniques), 58
  suffixes at 0x08051660. Inventory slots 0..6 are worn (armor, necklace, cloak, shoes, helm,
  weapon, second hand: Gandalf keeps his staff there), 7..14 the 8-item backpack. Checked against the character screen (Defense 22 = 20 +
  Mantle + Shoes; Shield Block 5% = Amulet). `ttt_save.py --rom` names every item.
- **Stats:** stat k is named by text 271 + k: 0 damage, 1 2H damage, 2 fire, 5..10 vs foe types,
  11 reflect, 12 two-handed flag, 13 melee / 14 missile / 15 element / 16 critical armor,
  17 day / 18 night armor, 19..23 strength..courage, 24 HP, 25 spirit, 26 crit %, 27 speed %, ...
  52 all primary stats, 55 all skills. Close to ROTK's `enum Stat` but shifted (ROTK has
  strength at 17), so import converts stats through a table.

### Next steps for Phase 0

1. The remaining unknown slot bytes (0x54, 0x56, 0x57, 0x5C, 0x62..0x64) and the other hero ids.
2. Confirm the two-prefix item layout with an affixed item (all items seen so far are plain).
3. Skill order for the other heroes (one save each), then the import tables:
   Two Towers base item -> ROTK item type/base, stat -> ROTK stat, skill -> ROTK or page 3 skill.

## Import (Two Towers -> ROTK, one way)

Import only goes from Two Towers to ROTK. There is no export back.

Everything carries over 1:1; nothing is converted to a ROTK equivalent.

- **Hero:** ROTK's `HeroId` 0..5 are Two Towers' heroes in the same order (Frodo, Legolas,
  Aragorn, Gandalf, Eowyn, Gimli), so the id copies directly.
- **Level, XP, attributes, points, gems, kills:** copied. Both games use the same 50-step XP
  table and save only the XP since the last level-up, but ROTK stores the displayed level minus
  one, so its next level costs one table step less. The import keeps the displayed level and the
  same share of the way to the next one (Gandalf 3, 488/1000 -> 292/600).
- **Items: legacy items.** The Two Towers items arrive as they are, with Two Towers' own names,
  stats and icons, read from the Two Towers ROM at runtime (the PC build holds both games).
  ROTK never drops them; they exist only through import. Worn slots map onto ROTK's: armor,
  necklace->jewelry, cloak, shoes->boots, helm->helmet, weapon->mainhand, second hand->offhand.
  The backpacks are both 8 slots.
- **Skills: legacy skills on page 3,** the hero's 11 Two Towers skills ported 1:1 (same costs,
  scaling and level requirements). Their levels copy from the save.

### Legacy item format

A ROTK item is 4 bytes and all 16 item types are taken; a Two Towers item (2 prefixes, base,
suffix) needs 29 bits. So the ROTK item is a handle:

- `itemType = UNIQUE`, `baseIndex = 0xFE` (past ROTK's 53 uniques), rune `NONE`, and the
  prefix/suffix bytes hold a 16-bit index into the legacy item pool.
- **Legacy item pool:** a new region of the Expanded SRAM save, global rather than per slot, because
  the ranger hollow stash is shared between characters. Each entry holds the 4 original Two
  Towers bytes. An import appends its items; entries are never reused, so a handle stays valid
  wherever the item goes (backpack, hollow, sold items just leave an unused entry).
- **Legacy skill records:** a per-slot region beside skill_expansion's: hero, 11 skill levels and
  a tag, 16 bytes per slot.
- Both regions are reserved in the mod's `save_layout.h` (region 7 at 0x5FAC, region 8 at 0x642C).
- **In-game import:** on the Load Game menu, L on an empty slot shows a Two Towers hero from the
  save beside the Two Towers ROM (Left/Right picks another, A imports, B cancels). The mod builds
  a fresh ROTK character of the same hero (`player_initHeroStats`/`player_initHeroLoadout`),
  gives it the Two Towers items as legacy items, saves it through `save_serializePlayer` and then
  lays the Two Towers level, stats and points over the slot.
- **Importer tool:** `two-towers/tools/ttt_import.py` does the same offline: it lays a Two Towers slot over a
  freshly created ROTK character of the same hero and writes a new .sav.
- **Room:** the save has 8276 bytes free after region 6. 72 slots x 16 bytes of skill records
  plus a 1024-entry pool (4 KB) leaves about 3 KB spare.
- In memory a legacy item is a real ROTK item (see "Legacy items in memory"). A legacy stat with a
  ROTK equivalent applies as that stat. A Two Towers skill bonus (+Wisdom, +all passive skills,
  ...) is dropped until page 3 exists.

## Porting the Two Towers skills 1:1

Goal: each legacy skill behaves as in The Two Towers (numbers, effects, timing, visuals), not as its
closest ROTK skill. `two-towers/tools/ttt_dis.py ADDRESS [LENGTH]` disassembles Two Towers code with the ROTK
names the matcher found.

How a Two Towers active runs:
- Cast: per hero, 11 cast functions at 0x08057AB0 + hero * 0x2C (hero ids: 0 Frodo, 1 Legolas,
  2 Aragorn, 3 Gandalf, 4 Eowyn, 5 Gimli). Gandalf's Zap (0x0803BC78) checks Spirit, takes it
  (`player_addSpirit`), records the skill at player +0x1F, sets the casting flag and starts the
  cast pose.
- Trigger: the player actor tick (0x0801AFC8) switches on the actor's character type (+0x22, 0x0A
  for Gandalf) and the cast skill to an effect function; another per-hero switch (0x0801C370,
  hero -> 0x0801F590/F520/F4D4/F5E8/F6C8/F664) ends the cast.
- Gandalf's effects: Shield 0x080185B4, Zap 0x08018FD4, Sunburst 0x08018880, Fireworks
  0x08018280, Summon Eagle 0x08017AD4 + 0x080180CC, Thunderstrike 0x0801E658. Magical Healing
  acts in its cast function.
- Skill levels: player +0x30 + skill index (Two Towers player stride 0x128).

Zap (0x08018FD4): a projectile actor at the caster's hotspot, speed (per-facing table 0x083278DC)
x 0x50000, 30-frame lifetime, damage = 5 + 2 x level (+0x90), animation 0x08328258[facing],
palette 0x08CC6268, tick 0x080191F1, hit callback 0x080115E1, a hit box object, sound 0x56.

Per skill the port needs the whole chain read (cast, effect, the effect actor's callbacks and its
damage), the effect rebuilt from ROTK engine calls, its sprite converted like the item icons, and
its sound matched. About 36 actives and 30 passives.

### Skill port status (2026-10-28)

Ported (rotk `hacks/patches/legacy_skills/legacy_skills.c`, art via `kSprites` in
`src/two_towers.cpp`): Gandalf's Zap, Magical Healing, Shield, Sunburst (no rays yet: ROTK sprites
do not rotate). Fireworks flies its 30 ticks in open ground and is gone without a blast at a wall
(the map edge, x 17, in the earlier test); The Two Towers' rocket also has tile collision. A hit on
a foe is not verified yet: in the stress-spawn test the aimed rocket (and Zap) flew past the foes.
Still stand-ins: Summon Eagle (0x08017AD4 + 0x080180CC), Thunderstrike (0x0801E658). Then
Gandalf's passives exact (Flame of Anor's revive burst is Sunburst's second branch, skill 3 level),
then the other heroes. Sounds and cast poses are ROTK stand-ins throughout.

Buying (rotk 63e69d4): the page shows for the six Two Towers heroes, imported or not. A buys a
rank with the shared skill point by The Two Towers' rules (its table at 0x08057780: 0x88 bytes
per hero, u16 text id, then 8 bytes per skill: tile x, y, two unknown bytes, the first rank's
player level, the prerequisite skill or 0xFF; checked at 0x08039720): requirements gate the
first rank only, max rank 20. Imported levels are the start. Casts read the full level.
Single player only. Sam, Smeagol, the Ringwraith and Saruman get no page.

Lessons:
- Spawn effects with `fx_unknown08043d74` (ROTK's projectile spawner) and override; play Two
  Towers art with `setSprite`, which clears the spawner's animation script (else the animation
  never ends).
- Do not set `renderFollowTarget` on a fresh effect (render sort hangs); copy `coords` instead.
- Do not borrow Herbal Healing's row (cast hangs).
- Two Towers actor offsets differ from ROTK's (state 0x67 vs 0x62, timer 0x58 vs 0x50, facing 0xFA
  vs 0xF8, radius 0x75 vs 0x72); flags, stun state 0x1A, hit mask 0x10040400 match.
- Effects tick at 30 Hz; Two Towers counts the same ticks.
- Two Towers sprite frames: 6-byte head, hotspots, 8-byte trigger boxes, 12-byte pieces; some
  animations are 8-byte records (tiles, frames) bound without a duration.
- Testing: `ROTK_PC_STRESS_SPAWN=N` spawns foes; the debug cheats byte is save offset 0x2DAD (3 =
  max level and infinite skill points); legacy records start at 0x5FAC; the save `allskills.sav` (scratch) has all of
  Gandalf's actives at level 2; actor pool and player state are read from `final.gbas` (host RAM
  at the "RPC1" tag, IWRAM player at offset 0x400FC + 0x3C00).

## Technical notes

- **Mod RAM:** the mod's globals live in a fixed 2.7 KB EWRAM slice (`hacks/layout.cfg`
  MODCODE_BSS), full after Versus. The PC edition of the mod (`make hack-pc`, `PC_EDITION=1`)
  links them at 0x12001000 in host RAM instead, with 64 KB reserved by the PC port before its
  other host RAM regions; `modRam_init` clears them at boot and the stock heap gets the whole
  EWRAM arena back. The Expanded build compiles this edition.
- **Two Towers data for the mod:** the PC port finds The Two Towers ROM (`ROTK_PC_TWO_TOWERS_ROM`,
  else beside the ROTK ROM or the executable, SHA-1 checked) and copies a 93 KB block into host
  RAM before the heap: the three item tables, the item, stat, hero and skill text decoded for
  ROTK's 5 languages, and the character slots of the .sav beside that ROM (`src/two_towers.h`). Info block version 4 points at it
  (`PcHostInfo.twoTowers`, `hackLib_twoTowersData()`). Only this block goes into host RAM, not the
  16 MB ROM, so save states and rewind stay small. `RotkTwoTowersCheck <ROM>` checks it.
- **Legacy item tables:** the PC port copies `ItemTypeInfos` and every item table it points to
  into host RAM with 256 rows each and redirects the ROM literals that point at them
  (`src/legacy_item_tables.h`). Rows up to 0xBF copy the ROM (Ringwraith gear reaches 0xAF), rows
  0xC0..0xFD are legacy rows. A 4 KB scratch area before them holds the mod's bookkeeping.
- **Legacy items in memory:** the save keeps legacy handles. When a slot or hollow page loads, the
  `legacy_items` patch turns each handle into a real ROTK item of the matching type whose base,
  prefix and suffix rows it fills from the Two Towers records: Two Towers stats mapped to ROTK
  stats, both prefixes merged into one row, the icon and equip flags of a matching ROTK base item
  (Two Towers' own 2H flag wins), and names through `text_overrides` (text ids 0x5000 + Two Towers
  id). Stock code then handles icons, equipping, stats and names. Saving turns them back into
  handles. Up to 62 legacy items can be loaded in one session.
- **Tested (PC test build, 2026-10-28):** an imported Gandalf loads with every legacy item worn or
  in the backpack, with icons, Two Towers names and stats (Shoes +1 Defense, Cape +1 Missile
  Armor). The in-game import was tested end to end (preview, import, load, the saved slot's handles,
  pool and skill record), and so were legacy passives (Gandalf with Elusion 2, Wisdom 3 and Flame
  of Anor 1 gets corruption resist 24, Wisdom 3, revive 20). Not yet tested: the ranger hollow,
  co-op, a mid-mission save.
- **Legacy item icons:** the PC port redraws each Two Towers backpack icon in ROTK's style
  (`convertIcon` in `src/two_towers.cpp`): frame 0 of the base item's SpriteAnimation (ROM
  0x080519F0) is a 24x24 icon whose tile stream uses ROTK's LZ77 commands without their length
  biases and whose OAM pieces are 12 bytes ({s16 x, s16 y, u8 size, u8 shape, u8 w, u8 h, u32
  tile byte offset}). The gold bevel and the green fill are cleared, the art is centered in
  ROTK's 32x24 white and black frame, and the tiles are stored uncompressed. Base item 63's icon
  is broken in the ROM and no item uses it.
- **Legacy skills:** `legacy_skills` applies the passives as their closest ROTK stats from the
  slot's record (single player). Some have no ROTK mechanic and use the nearest stat (Meditation,
  Spirit of the Forge, Duck). Page 3 and active legacy skills are still to do. Their state can
  live in the host scratch like the legacy items', so the mod RAM does not have to move first.
- **Page 3 (done for Gandalf, PC edition):** legacy actives are active rows 13..19
  (`SKILL_EXPANSION_ACTIVE_ROW_CAPACITY` 20; saves and co-op keep `SKILL_EXPANSION_ACTIVE_ROW_COUNT`
  13). Each runs its ROTK counterpart's stock callback with the Two Towers Spirit cost and level
  (capped at five). The skill tree's third page, "The Two Towers", steps in from page 1 (left) or
  page 2 (right): six actives on the active grid (swapped in like page 2), the passives and a
  seventh active on the passive grid, Two Towers text as descriptions, nothing to buy. The record
  follows the character on save and is cleared on delete. Two Towers skill icons: per hero 11 BG
  assets at 0x0832EA64 packed with the C64 pucrunch (ARM decoder at 0x08000264, ported to C++ and
  checked against it in unicorn); the art is mapped by brightness onto ROTK's icon shades.
  Still to do: Fireworks and Thunderstrike cast stand-ins (Lightstrike, Blinding Aura); the other
  heroes' actives have no mapping yet; quick-select rows 13+ are not kept by the Expanded save;
  legacy skills are off in co-op.
- **Mod ↔ PC port channel:** `PcHostInfo` in host RAM (version 4 adds the Two Towers data, the
  legacy item tables and the scratch). The menu button's
  switch request and the import hand-off can use the same block.
- **Game handoff precedent:** `src/expanded_edition.cpp` already hands a session to another build;
  the super game should run both carts in one exe instead.
