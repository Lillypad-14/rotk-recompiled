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
| 0 | **Map Two Towers** with ROTK's names | `tools/ttt_match.py`, `tools/ttt_save.py` (player and save mapped, below) |
| 1 | **Two Towers on PC** (recompile, launcher, the PC extras) | biggest risk: no decomp, expect crash-fix iterations |
| 2 | **Menu button + seamless switch** | mod adds the button; the PC port swaps carts (both games in one exe) |
| 3 | **Character import** | ROTK save format is known; needs the TTT player/save layout |
| 4 | **Legacy skills page 3**, hero by hero | per-hero kit work, like an Expanded kit |
| 5 | **ROTK sprites in Two Towers** (toggle) | shared sprite engine, mostly data |
| later | Two Towers split-screen co-op | TTT has the same link system (`g_MpLinkState`) |

## Phase 0 findings (2026-10-28)

Two Towers ROM: "TWO TOWERS", code ALPE, 16 MB, SHA-1 `82b3b4afb0ed0d3fca646aa266e0bfc5bffd49d9`,
EEPROM save (`EEPROM_V122`), same Griptonite engine as ROTK.

`python tools/ttt_match.py --ttt "<TTT ROM>"` (about 8 minutes) writes `.private/ttt/symbols.json`:

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
as ROTK's does. `python tools/ttt_save.py "<TTT .sav>"` decodes it; the slot fields are listed there.

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
  `.private/ttt/text_en.tsv`.
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
- **Importer:** `tools/ttt_import.py` lays a Two Towers slot over a freshly created ROTK character
  of the same hero (the in-game import will do the same: create the hero, then apply the Two
  Towers data), writing a new .sav. `--no-items` keeps the template's gear for builds that cannot
  show legacy items yet; the current build would read a legacy handle as an out-of-range unique.
- **Room:** the save has 8276 bytes free after region 6. 72 slots x 16 bytes of skill records
  plus a 1024-entry pool (4 KB) leaves about 3 KB spare.
- The mod hooks the item name, icon, stat and equip lookups for the handle. A legacy stat with a
  ROTK equivalent applies as that stat. A Two Towers skill bonus (+Wisdom, +all passive skills,
  ...) applies to the page 3 skill and does nothing until page 3 exists.

## Technical notes

- **Mod RAM:** the mod's globals live in a fixed 2.7 KB EWRAM slice (`hacks/layout.cfg`
  MODCODE_BSS), now full after Versus. For the PC edition, move them to host RAM (0x12000000+,
  see `hacks/lib/pc_host.h` / `src/main.cpp` host RAM layout) with a PC build setting. Needed
  before page 3.
- **Two Towers data for the mod:** the PC port finds The Two Towers ROM (`ROTK_PC_TWO_TOWERS_ROM`,
  else beside the ROTK ROM or the executable, SHA-1 checked) and copies a 92 KB block into host
  RAM after the heap: the three item tables and the item, stat, hero and skill text decoded for
  ROTK's 5 languages (`src/two_towers.h`). Info block version 4 points at it
  (`PcHostInfo.twoTowers`, `hackLib_twoTowersData()`). Only this block goes into host RAM, not the
  16 MB ROM, so save states and rewind stay small. `RotkTwoTowersCheck <ROM>` checks it.
- **Legacy items in memory:** the save keeps legacy handles. When a slot or hollow page loads, the
  mod turns each handle into a real ROTK item of the matching type whose base, prefix and suffix
  rows it builds in host RAM from the Two Towers records (Two Towers stats mapped to ROTK stats,
  both prefixes merged into one row), the way the Ringwraith gear extends the item tables. Stock
  code then handles icons, equipping, stats and names. Saving turns them back into handles.
- **Mod ↔ PC port channel:** `PcHostInfo` in host RAM (version 3 adds `duel`). The menu button's
  switch request and the import hand-off can use the same block.
- **Game handoff precedent:** `src/expanded_edition.cpp` already hands a session to another build;
  the super game should run both carts in one exe instead.
