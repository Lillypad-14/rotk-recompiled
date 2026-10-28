"""Import a Two Towers hero into an Expanded ROTK save (one way: Two Towers -> ROTK).

The Two Towers slot is laid over a fresh ROTK character of the same hero (the template slot), the
way the in-game import will create a hero and then apply the Two Towers data: everything ROTK-only
(skill data, runes, mission start, whetstone) stays as character creation left it.

    python tools/ttt_import.py "<Two Towers .sav>" --ttt-slot 2 --rotk-sav "<Expanded .sav>" \\
        --template-slot 0 [--slot N] [--no-items] --out "<new .sav>"

Carried over: hero, level (as displayed), XP (same share of the level), current HP and spirit,
gems, kills, unspent attribute and skill points, base attributes, items and skill levels. Items
become legacy item handles into the legacy item pool and the skill levels a legacy skill record
(docs/SUPER_GAME.md, "Legacy item format"); --no-items leaves the inventory as the template had it,
for builds that cannot show legacy items yet. The input saves are never modified.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ttt_save  # noqa: E402

SRAM_SIZE = 0x8000
SLOT_SIZE = 0x70
SLOT_COUNT = 72                    # 18 pages x 4
SLOT_BASE = 0x40                   # after the 0x40-byte SaveGlobals header
HEADER_MAGIC = b"ROTKRCM"
SLOT_MISSION_EMPTY = 0x82

# ROTK SaveSlot fields (include/save.h)
EXPERIENCE, CURRENT_HP, CURRENT_SPIRIT, GEMS, KILLS = 0x40, 0x4C, 0x4E, 0x50, 0x52
LEVEL, ATTRIBUTE_POINTS, SKILL_POINTS, ATTRIBUTES, HERO = 0x5C, 0x5D, 0x5E, 0x5F, 0x65
MISSION, CHECKSUM = 0x68, 0x6E

# Both games level up with the same table, but ROTK stores the displayed level minus one.
XP_TO_NEXT = ttt_save.XP_TO_NEXT

# Two Towers worn slots 0..6 (armor, necklace, cloak, shoes, helm, weapon, second hand) in ROTK's
# Inventory order (armor, mainhand, offhand, helmet, cloak, gloves, boots, jewelry, backpack 0..7).
WORN_TO_ROTK = (0, 7, 4, 6, 3, 1, 2)
BACKPACK_ROTK = 8

# Legacy regions after skill_expansion's region 6 (save_layout.h).
LEGACY_SKILLS_OFF = 0x5FAC         # region 7: 72 x 16-byte legacy skill records
LEGACY_SKILL_RECORD = 16
LEGACY_POOL_OFF = LEGACY_SKILLS_OFF + SLOT_COUNT * LEGACY_SKILL_RECORD  # region 8
LEGACY_POOL_MAGIC = b"TTLG"
LEGACY_POOL_ENTRIES = 1024
LEGACY_RECORD_TAG = 0x01
LEGACY_ITEM_BYTE0 = 0xFE           # itemType UNIQUE (0xE), rune NONE (0xF)
LEGACY_ITEM_BASE = 0xFE            # past ROTK's 53 uniques


def checksum(slot: bytes) -> int:
    return sum(struct.unpack(f"<{len(slot) // 2}H", slot)) & 0xFFFF


def slot_offset(index: int) -> int:
    return SLOT_BASE + index * SLOT_SIZE


def slot_used(image: bytes, index: int) -> bool:
    slot = image[slot_offset(index):slot_offset(index) + SLOT_SIZE]
    return checksum(slot) == 0 and struct.unpack_from("<H", slot, MISSION)[0] < SLOT_MISSION_EMPTY


def rotk_level_and_xp(level: int, experience: int) -> tuple[int, int]:
    """ROTK's stored level and XP for a Two Towers level: the same displayed level and the same
    share of the way to the next one (ROTK's next level costs one table step less)."""
    stored = max(level - 1, 0)
    if level >= len(XP_TO_NEXT):
        return stored, 0
    return stored, min(experience * XP_TO_NEXT[stored] // XP_TO_NEXT[level], XP_TO_NEXT[stored] - 1)


def pool_append(image: bytearray, items: list[bytes]) -> list[int]:
    """Add Two Towers items to the legacy item pool and return their indexes."""
    header = image[LEGACY_POOL_OFF:LEGACY_POOL_OFF + 8]
    count = struct.unpack_from("<H", header, 4)[0] if header[:4] == LEGACY_POOL_MAGIC else 0
    if count + len(items) > LEGACY_POOL_ENTRIES:
        sys.exit("the legacy item pool is full")
    image[LEGACY_POOL_OFF:LEGACY_POOL_OFF + 8] = LEGACY_POOL_MAGIC + struct.pack("<HH", count + len(items), 0)
    indexes = []
    for item in items:
        at = LEGACY_POOL_OFF + 8 + count * 4
        image[at:at + 4] = item
        indexes.append(count)
        count += 1
    return indexes


def import_slot(ttt: dict, image: bytearray, template: int, target: int, items: bool) -> None:
    slot = bytearray(image[slot_offset(template):slot_offset(template) + SLOT_SIZE])
    if slot[HERO] >> 4 != ttt["heroId"]:
        sys.exit(f"the template slot is hero {slot[HERO] >> 4}, the Two Towers hero is {ttt['heroId']}")
    if slot[LEVEL] != 0 or struct.unpack_from("<I", slot, EXPERIENCE)[0] != 0:
        sys.exit("the template slot has to be a freshly created character (level 1, no XP)")

    level, xp = rotk_level_and_xp(ttt["level"], ttt["experience"])
    struct.pack_into("<I", slot, EXPERIENCE, xp)
    struct.pack_into("<hhHH", slot, CURRENT_HP, ttt["currentHp"], ttt["currentSpirit"], ttt["gems"], ttt["killCount"])
    slot[LEVEL], slot[ATTRIBUTE_POINTS], slot[SKILL_POINTS] = level, ttt["attributePoints"], ttt["skillPoints"]
    slot[ATTRIBUTES:ATTRIBUTES + 5] = bytes.fromhex(ttt["baseAttributes"])

    if items:
        inventory = bytes.fromhex(ttt["inventory"])
        carried = [(k, inventory[4 * k:4 * k + 4]) for k in range(len(inventory) // 4)]
        carried = [(k, item) for k, item in carried if item != ttt_save.EMPTY_ITEM]
        indexes = pool_append(image, [item for _, item in carried])
        slot[0:0x40] = ttt_save.EMPTY_ITEM * 16
        for (k, _), index in zip(carried, indexes):
            at = WORN_TO_ROTK[k] if k < len(WORN_TO_ROTK) else BACKPACK_ROTK + k - len(WORN_TO_ROTK)
            slot[4 * at:4 * at + 4] = bytes((LEGACY_ITEM_BYTE0, LEGACY_ITEM_BASE)) + struct.pack("<H", index)

    struct.pack_into("<H", slot, CHECKSUM, 0)
    struct.pack_into("<H", slot, CHECKSUM, -checksum(slot) & 0xFFFF)
    image[slot_offset(target):slot_offset(target) + SLOT_SIZE] = slot

    record = bytes((LEGACY_RECORD_TAG, ttt["heroId"])) + bytes.fromhex(ttt["skillLevels"])
    at = LEGACY_SKILLS_OFF + target * LEGACY_SKILL_RECORD
    image[at:at + LEGACY_SKILL_RECORD] = record.ljust(LEGACY_SKILL_RECORD, b"\xff")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ttt_sav", type=Path)
    parser.add_argument("--ttt-slot", type=int, required=True, help="Two Towers slot, 0..3")
    parser.add_argument("--rotk-sav", type=Path, required=True, help="an Expanded ROTK save (32 KB SRAM)")
    parser.add_argument("--template-slot", type=int, required=True,
                        help="a freshly created ROTK character of the same hero")
    parser.add_argument("--slot", type=int, help="the ROTK slot to write (default: the first empty one)")
    parser.add_argument("--no-items", action="store_true", help="keep the template's inventory")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    ttt = ttt_save.read_save(args.ttt_sav)["slots"][args.ttt_slot]
    if not ttt["used"]:
        sys.exit(f"Two Towers slot {args.ttt_slot} is empty")
    image = bytearray(args.rotk_sav.read_bytes())
    if len(image) != SRAM_SIZE or not image.startswith(HEADER_MAGIC):
        sys.exit("not an Expanded ROTK save (32 KB, ROTKRCM header)")
    if not slot_used(image, args.template_slot):
        sys.exit(f"ROTK slot {args.template_slot} is empty")
    target = args.slot
    if target is None:
        target = next((n for n in range(SLOT_COUNT) if not slot_used(image, n)), None)
        if target is None:
            sys.exit("no empty ROTK slot")

    import_slot(ttt, image, args.template_slot, target, not args.no_items)
    if args.out.resolve() in (args.rotk_sav.resolve(), args.ttt_sav.resolve()):
        sys.exit("--out has to be a new file")
    args.out.write_bytes(image)
    page, row = divmod(target, 4)
    level, xp = rotk_level_and_xp(ttt["level"], ttt["experience"])
    print(f"{ttt_save.HEROES.get(ttt['heroId'], ttt['heroId'])} level {ttt['level']} -> ROTK slot {target} "
          f"(page {page + 1}, row {row + 1}), stored level {level}, xp {xp}; wrote {args.out}")


if __name__ == "__main__":
    main()
