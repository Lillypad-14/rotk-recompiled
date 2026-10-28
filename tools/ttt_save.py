"""Decode The Two Towers GBA save (.sav, 512-byte EEPROM) into its character slots.

The layout comes from Two Towers' save_serializePlayer (0x0803532C, found by tools/ttt_match.py):
4 slots of 0x78 bytes at EEPROM blocks 4/19/34/49, each a copy of the active Player's fields
plus the mission state, closed by a negated halfword checksum (the slot sums to 0). The .sav
stores every 8-byte EEPROM block byte-reversed, as ROTK's does.

    python tools/ttt_save.py "<Two Towers .sav>" [--rom "<Two Towers ROM>"] [--json]

With --rom, items and their stats are named from the ROM's tables and Huffman text (decoded with
ROTK's rotkit from ../rotk/tools).

Field names follow ROTK's SaveSlot where the Player field matches (offsets in the Two Towers
Player struct are in brackets); unknown_* fields are copied but not yet identified.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
ROM_BASE = 0x08000000

BLOCK = 8
SLOT_SIZE = 0x78
SLOT_START_BLOCKS = (4, 19, 34, 49)  # ROM 0x0805765C
HEADER_MAGIC = b"LORGBA03"

HEROES = {0: "Frodo", 1: "Legolas", 2: "Aragorn", 3: "Gandalf", 4: "Eowyn", 5: "Gimli"}  # text 18..23

# XP from each level to the next, indexed by level (ROM 0x08050348, player_awardXp 0x0800D8FC).
XP_TO_NEXT = (100, 300, 600, 1000, 1500, 2100, 2800, 3600, 4500, 5500, 6600, 7800)

# Items are 4 bytes: prefix, second prefix, base, suffix (0xFF = none; all 0xFF = empty slot).
# Each table holds 16-byte records: u16 name text id, s16 gem value, then up to 5 (stat, s8 value)
# pairs with stat 0x40 = none. Stat id k is named by text 271 + k.
LANGUAGE_PACKAGES = 0x0832EBA4  # TextPackage *[], read by text_setLanguage 0x0803E9F8
PREFIXES, PREFIX_COUNT = 0x080504F0, 152
BASES, BASE_COUNT = 0x08050E70, 127   # one table for every item type; the last 21 are uniques
SUFFIXES, SUFFIX_COUNT = 0x08051660, 58
STAT_NAMES, STAT_NONE = 271, 0x40
EMPTY_ITEM = bytes([0xFF] * 4)
EQUIPMENT_SLOTS = 7  # inventory 0..6 are worn: armor, necklace, cloak, shoes, helm, weapon, shield/off-hand

# Each hero's 11 skill descriptions, in skillLevels order (passives first, then actives); Gimli's
# block comes before Eowyn's. Checked on Gandalf: his starting Magical Healing is byte 4.
SKILL_TEXT = {0: 348, 1: 359, 2: 370, 3: 381, 4: 403, 5: 392}

# (slot offset, struct format, name)
FIELDS = [
    (0x00, "60s", "inventory"),          # [0xBC] 0x3C bytes; ROTK's Inventory is 0x40
    (0x3C, "11s", "skillLevels"),        # [0x25..0x2F]
    (0x47, "B", "flags"),                # [0x100] bit 1: level-up notice pending
    (0x48, "I", "experience"),           # [0x04] XP into the current level (see XP_TO_NEXT)
    (0x4C, "h", "currentHp"),            # [0x08]
    (0x4E, "h", "currentSpirit"),        # [0x0A]
    (0x50, "H", "gems"),                 # [0x0C]
    (0x52, "H", "killCount"),            # [0x0E]
    (0x54, "H", "unknown_54"),           # [0x10]
    (0x56, "B", "unknown_56"),           # [0x14] capped at 100 (ROTK's player_addCorruption)
    (0x57, "B", "unknown_57"),           # [0x15]
    (0x58, "B", "level"),                # [0x16]
    (0x59, "B", "attributePoints"),      # [0x17] +5 per level
    (0x5A, "B", "skillPoints"),          # [0x18] one pool for both pages; +1 per level and per 500 kills
    (0x5B, "B", "heroId"),               # [0x1A] 3 = Gandalf
    (0x5C, "B", "unknown_5c"),           # [0x1E]
    (0x5D, "5s", "baseAttributes"),      # [0xFE..0xFA] strength, accuracy, health, defense, courage
    (0x62, "3s", "unknown_62"),          # [0x22..0x24]
    (0x65, "B", "variant"),              # table at 0x030038A0 indexed by mission
    (0x66, "H", "mission"),
    (0x68, "H", "checkpoint_68"),        # 0x030039C0
    (0x6A, "H", "checkpoint_6a"),        # 0x030039C4
    (0x6C, "B", "checkpoint_6c"),        # 0x030039C8
    (0x76, "H", "checksum"),
]


def unswap(data: bytes) -> bytes:
    """EEPROM image in the game's byte order (each 8-byte block reversed in the .sav)."""
    return b"".join(data[i:i + BLOCK][::-1] for i in range(0, len(data), BLOCK))


def checksum_ok(data: bytes) -> bool:
    return sum(struct.unpack(f"<{len(data) // 2}H", data)) & 0xFFFF == 0


def decode_slot(raw: bytes) -> dict:
    out = {}
    for offset, fmt, name in FIELDS:
        (value,) = struct.unpack_from("<" + fmt, raw, offset)
        out[name] = value.hex() if isinstance(value, bytes) else value
    return out


def total_experience(level: int, experience: int) -> int:
    """Lifetime XP: the save keeps only the XP earned since the last level-up."""
    return sum(XP_TO_NEXT[1:level]) + experience


class Items:
    """Names and stats of Two Towers items, from the ROM."""

    def __init__(self, rom: bytes):
        sys.path.insert(0, str(ROOT.parent / "rotk/tools"))
        from rotkit import textdb
        from rotkit.text_encoding import game_to_unicode
        package = struct.unpack_from("<I", rom, LANGUAGE_PACKAGES - ROM_BASE)[0]
        self.rom = rom
        self.text = lambda sid: game_to_unicode(textdb.decode(rom, package, sid)[0])

    def record(self, table: int, index: int) -> tuple[str, list[tuple[str, int]]]:
        at = table - ROM_BASE + index * 16
        name_id = struct.unpack_from("<H", self.rom, at)[0]
        stats = []
        for k in range(5):
            stat, value = struct.unpack_from("<Bb", self.rom, at + 4 + k * 2)
            if stat != STAT_NONE:
                stats.append((self.text(STAT_NAMES + stat), value))
        return self.text(name_id), stats

    def skill(self, hero: int, index: int) -> str:
        """A skill's name and required level, e.g. "Zap (Level 2)"."""
        return self.text(SKILL_TEXT[hero] + index).split(" - ")[0]

    def describe(self, item: bytes) -> str:
        if item == EMPTY_ITEM:
            return ""
        parts, stats = [], []
        for byte, table, count in ((item[0], PREFIXES, PREFIX_COUNT), (item[1], PREFIXES, PREFIX_COUNT),
                                   (item[2], BASES, BASE_COUNT), (item[3], SUFFIXES, SUFFIX_COUNT)):
            if byte != 0xFF:
                if byte >= count:
                    return f"? ({item.hex()})"
                name, more = self.record(table, byte)
                parts.append(name)
                stats += more
        return " ".join(parts) + "  [" + ", ".join(f"{name} {value:+d}" for name, value in stats) + "]"


def read_save(path: Path) -> dict:
    image = unswap(path.read_bytes())
    header = image[:4 * BLOCK]
    slots = []
    for index, block in enumerate(SLOT_START_BLOCKS):
        raw = image[block * BLOCK:block * BLOCK + SLOT_SIZE]
        used = any(raw) and checksum_ok(raw)
        slot = dict(index=index, used=used)
        if used:
            slot.update(decode_slot(raw))
            slot["totalExperience"] = total_experience(slot["level"], slot["experience"])
        slots.append(slot)
    return dict(header_ok=header.startswith(HEADER_MAGIC) and checksum_ok(header), slots=slots)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sav", type=Path)
    parser.add_argument("--rom", type=Path, help="the Two Towers ROM, to name items")
    parser.add_argument("--json", action="store_true", help="print the decoded save as JSON")
    args = parser.parse_args()
    save = read_save(args.sav)
    items = Items(args.rom.read_bytes()) if args.rom else None
    if args.json:
        print(json.dumps(save, indent=1))
        return
    print(f"header {'ok' if save['header_ok'] else 'BAD'}")
    for slot in save["slots"]:
        if not slot["used"]:
            print(f"slot {slot['index']}: empty")
            continue
        print(f"slot {slot['index']}: hero {HEROES.get(slot['heroId'], slot['heroId'])} level {slot['level']} xp {slot['experience']} "
              f"hp {slot['currentHp']} spirit {slot['currentSpirit']} gems {slot['gems']} mission {slot['mission']}")
        for name, value in slot.items():
            if name not in ("index", "used") and not (items and name in ("inventory", "skillLevels")):
                print(f"    {name:16} {value}")
        if items:
            levels = bytes.fromhex(slot["skillLevels"])
            learned = [f"{items.skill(slot['heroId'], k)} = {v}" for k, v in enumerate(levels) if v]
            print(f"    skills           {', '.join(learned) or 'none'}")
            inventory = bytes.fromhex(slot["inventory"])
            for k in range(len(inventory) // 4):
                where = "worn" if k < EQUIPMENT_SLOTS else "pack"
                print(f"    {where} {k:2}  {items.describe(inventory[4 * k:4 * k + 4])}")


if __name__ == "__main__":
    main()
