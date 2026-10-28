"""Label The Two Towers GBA ROM with the ROTK decomp's names.

Both games run Griptonite's engine. Each named ROTK function is matched in the Two Towers ROM by
its bytes, with the parts that differ between builds masked: BL call targets and the pointer words
of literal pools. Matches then spread through the call graph (a matched function's calls at the
same offsets name the callees). Both games link their code in the same order, so the functions
between two matched neighbours are then aligned by instruction shape (opcodes with registers and
immediates masked), which finds functions whose bodies changed. Literal-pool loads at the same
place in the aligned instructions name RAM globals.

    python tools/ttt_match.py --rotk game/rotk.gba --symbols ../rotk/build/game_symbols.inc --ttt <TTT ROM>

Writes .private/ttt/symbols.json (functions and globals with their evidence) and prints a summary.
"""

from __future__ import annotations

import argparse
import bisect
import difflib
import functools
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
ROM_BASE = 0x08000000
ANCHOR_HALVES = 6       # unmasked halfwords an anchor needs
MIN_SCORE = 0.9         # share of comparable halfwords that must match
MAX_FUNCTION = 0x1000   # longest body compared, in bytes
CODE_END = 0x08060000   # both games' code ends below this
MIN_SHAPE = 0.6         # instruction-shape similarity an aligned pair needs


def read_symbols(path: Path) -> tuple[list[tuple[int, str]], dict[int, str]]:
    functions, data = [], {}
    for line in path.read_text().splitlines():
        m = re.match(r"(\w+)\s+equ\s+0x([0-9a-f]+)\s*;\s*(\S*)", line)
        if not m:
            continue
        address = int(m.group(2), 16)
        if m.group(3) == "thumb" and address >= ROM_BASE:
            functions.append((address, m.group(1)))
        elif address < ROM_BASE:
            data[address] = m.group(1)
    functions.sort()
    return functions, data


def is_pointer(word: int) -> bool:
    return 0x02000000 <= word < 0x02040000 or 0x03000000 <= word < 0x03008000 or ROM_BASE <= word < 0x0A000000


class Rom:
    def __init__(self, data: bytes):
        self.data = data

    def half(self, address: int) -> int:
        return struct.unpack_from("<H", self.data, address - ROM_BASE)[0]

    def word(self, address: int) -> int:
        return struct.unpack_from("<I", self.data, address - ROM_BASE)[0]

    def signature(self, start: int, size: int) -> list[int | None]:
        """Halfwords of a function with BL pairs and literal-pool pointer words as None."""
        size = min(size, MAX_FUNCTION) & ~1
        halves: list[int | None] = [self.half(start + i) for i in range(0, size, 2)]
        pools = set()
        for i, h in enumerate(halves):
            if h is not None and h >> 11 == 0x09:  # ldr rX, [pc, #imm]
                target = ((start + i * 2 + 4) & ~3) + (h & 0xFF) * 4
                if start <= target < start + size - 2 and is_pointer(self.word(target)):
                    pools.add((target - start) // 2)
        for i in range(len(halves) - 1):
            h, l = halves[i], halves[i + 1]
            if h is not None and l is not None and h >> 11 == 0x1E and l >> 11 == 0x1F:
                halves[i] = halves[i + 1] = None
        for i in pools:
            halves[i] = halves[i + 1] = None
        return halves

    def calls(self, start: int, size: int) -> dict[int, int]:
        """BL offsets within a function -> their targets."""
        out = {}
        for i in range(0, min(size, MAX_FUNCTION) - 2, 2):
            h, l = self.half(start + i), self.half(start + i + 2)
            if h >> 11 == 0x1E and l >> 11 == 0x1F:
                disp = ((h & 0x7FF) << 12) | ((l & 0x7FF) << 1)
                if disp & 0x400000:
                    disp -= 0x800000
                out[i] = start + i + 4 + disp
        return out

    def pool_words(self, start: int, size: int) -> dict[int, int]:
        """Literal-pool slot offsets within a function -> their pointer words."""
        out = {}
        for i in range(0, min(size, MAX_FUNCTION) & ~1, 2):
            h = self.half(start + i)
            if h >> 11 == 0x09:
                target = ((start + i + 4) & ~3) + (h & 0xFF) * 4
                if start <= target < start + size - 2:
                    word = self.word(target)
                    if is_pointer(word):
                        out[target - start] = word
        return out


def shape(h: int) -> int:
    """A Thumb instruction with its registers and immediates masked (conditions and ALU ops kept)."""
    t = h >> 11
    if t == 0x03:
        return h >> 9           # add/sub register or imm3
    if t == 0x08:
        return h >> 6 if not h & 0x400 else h >> 8  # ALU op / hi-register op
    if t in (0x0A, 0x0B):
        return h >> 9           # load/store register offset
    if t in (0x16, 0x17, 0x1A, 0x1B):
        return h >> 8           # push/pop/sp adjust, conditional branch
    return t


def code_starts(rom: Rom) -> set[int]:
    """Function starts: BL targets and Thumb pointers into the code."""
    halves = [h for (h,) in struct.iter_unpack("<H", rom.data[:CODE_END - ROM_BASE])]
    out = set()
    for i in range(len(halves) - 1):
        h, l = halves[i], halves[i + 1]
        if h >> 11 == 0x1E and l >> 11 == 0x1F:
            disp = ((h & 0x7FF) << 12) | ((l & 0x7FF) << 1)
            if disp & 0x400000:
                disp -= 0x800000
            target = ROM_BASE + i * 2 + 4 + disp
            if ROM_BASE <= target < CODE_END:
                out.add(target)
    for (word,) in struct.iter_unpack("<I", rom.data[:CODE_END - ROM_BASE]):
        if word & 1 and ROM_BASE <= word < CODE_END:
            out.add(word - 1)
    return out


def increasing(pairs: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """The longest run of (rotk, ttt) pairs whose ttt addresses rise with rotk's."""
    tails, ends, prev = [], [], [None] * len(pairs)
    for k, (_, t) in enumerate(pairs):
        i = bisect.bisect_left(tails, t)
        if i == len(tails):
            tails.append(t)
            ends.append(k)
        else:
            tails[i], ends[i] = t, k
        prev[k] = ends[i - 1] if i else None
    out, k = [], ends[-1] if ends else None
    while k is not None:
        out.append(pairs[k])
        k = prev[k]
    return out[::-1]


def align(rows: list[int], cols: list[int], sim) -> list[tuple[int, int, float]]:
    """The best order-keeping pairing of two function lists, by similarity."""
    s = [[sim(a, b) for b in cols] for a in rows]
    d = [[0.0] * (len(cols) + 1) for _ in range(len(rows) + 1)]
    for i in range(len(rows)):
        for j in range(len(cols)):
            step = s[i][j] if s[i][j] >= MIN_SHAPE else -1.0
            d[i + 1][j + 1] = max(d[i][j + 1], d[i + 1][j], d[i][j] + step)
    out, i, j = [], len(rows), len(cols)
    while i and j:
        if s[i - 1][j - 1] >= MIN_SHAPE and d[i][j] == d[i - 1][j - 1] + s[i - 1][j - 1]:
            out.append((rows[i - 1], cols[j - 1], s[i - 1][j - 1]))
            i, j = i - 1, j - 1
        elif d[i][j] == d[i - 1][j]:
            i -= 1
        else:
            j -= 1
    return out


def score(a: list[int | None], b: list[int | None]) -> float:
    same = total = 0
    for x, y in zip(a, b):
        if x is None or y is None:
            continue
        total += 1
        same += x == y
    return same / total if total else 0.0


def find(rotk: Rom, ttt: Rom, start: int, size: int) -> tuple[int, float] | None:
    """The Two Towers address of a ROTK function, from its rarest unmasked run, or None."""
    sig = rotk.signature(start, size)
    if len(sig) < 8:
        return None
    candidates: dict[int, float] = {}
    tried = 0
    for i in range(len(sig) - ANCHOR_HALVES):
        run = sig[i:i + ANCHOR_HALVES]
        if any(h is None for h in run):
            continue
        anchor = struct.pack(f"<{ANCHOR_HALVES}H", *run)
        hits, at = [], ttt.data.find(anchor)
        while at >= 0 and len(hits) < 8:
            if at % 2 == 0:
                hits.append(at)
            at = ttt.data.find(anchor, at + 1)
        tried += 1
        if 0 < len(hits) < 8:
            for hit in hits:
                begin = ROM_BASE + hit - i * 2
                if begin not in candidates and begin >= ROM_BASE:
                    candidates[begin] = score(sig, ttt.signature(begin, size))
        if tried >= 4 and candidates:
            break
    good = sorted(((s, a) for a, s in candidates.items() if s >= MIN_SCORE), reverse=True)
    if not good or (len(good) > 1 and good[1][0] == good[0][0]):
        return None
    return good[0][1], good[0][0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rotk", type=Path, default=ROOT / "game/rotk.gba")
    parser.add_argument("--symbols", type=Path, default=ROOT.parent / "rotk/build/game_symbols.inc")
    parser.add_argument("--ttt", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / ".private/ttt/symbols.json")
    args = parser.parse_args()
    rotk, ttt = Rom(args.rotk.read_bytes()), Rom(args.ttt.read_bytes())
    functions, data = read_symbols(args.symbols)
    starts = [a for a, _ in functions]
    names = {a: n for a, n in functions}

    def size_of(address: int) -> int:
        i = bisect.bisect_right(starts, address)
        return (starts[i] if i < len(starts) else address + 0x40) - address

    matched: dict[str, dict] = {}
    by_ttt: dict[int, str] = {}
    for address, name in functions:
        hit = find(rotk, ttt, address, size_of(address))
        if hit and hit[0] not in by_ttt:
            matched[name] = dict(rotk=address, ttt=hit[0], score=round(hit[1], 3), how="bytes")
            by_ttt[hit[0]] = name
    direct = len(matched)

    # Calls at the same offset in matched functions name their callees (thunks and changed bodies).
    changed = True
    while changed:
        changed = False
        for name, entry in list(matched.items()):
            size = size_of(entry["rotk"])
            mine, theirs = rotk.calls(entry["rotk"], size), ttt.calls(entry["ttt"], size)
            for offset, target in mine.items():
                callee = names.get(target)
                other = theirs.get(offset)
                if callee and other and callee not in matched and other not in by_ttt:
                    matched[callee] = dict(rotk=target, ttt=other, score=None, how=f"called from {name}")
                    by_ttt[other] = callee
                    changed = True

    # Functions between matched neighbours (both games keep the link order), aligned by shape.
    rotk_starts = sorted(set(starts) | code_starts(rotk))
    ttt_starts = sorted(code_starts(ttt) | set(by_ttt))

    def sized(table: list[int]):
        def size(address: int) -> int:
            i = bisect.bisect_right(table, address)
            return (table[i] if i < len(table) else address + 0x40) - address
        return size

    rotk_size, ttt_size = sized(rotk_starts), sized(ttt_starts)

    @functools.lru_cache(None)
    def shapes(rom: Rom, start: int, size: int) -> tuple[int, ...]:
        return tuple(shape(rom.half(start + i)) for i in range(0, min(size, 2 * MAX_FUNCTION) & ~1, 2))

    def similarity(a: int, b: int) -> float:
        sa, sb = rotk_size(a), ttt_size(b)
        if max(sa, sb) > 3 * min(sa, sb) + 16:
            return 0.0
        return difflib.SequenceMatcher(None, shapes(rotk, a, sa), shapes(ttt, b, sb), autojunk=False).ratio()

    anchors = increasing(sorted((e["rotk"], e["ttt"]) for e in matched.values()))
    pairs = dict(anchors)
    bounds = [(ROM_BASE, ROM_BASE)] + anchors + [(CODE_END, CODE_END)]
    for (r0, t0), (r1, t1) in zip(bounds, bounds[1:]):
        rows = [a for a in rotk_starts if r0 < a < r1]
        cols = [b for b in ttt_starts if t0 < b < t1]
        if rows and cols and len(rows) * len(cols) <= 40000:
            for a, b, sim in align(rows, cols, similarity):
                pairs[a] = b
                if a in names and names[a] not in matched and b not in by_ttt:
                    matched[names[a]] = dict(rotk=a, ttt=b, score=round(sim, 3), how="aligned")
                    by_ttt[b] = names[a]
    aligned = sum(1 for e in matched.values() if e["how"] == "aligned")

    # Literal-pool loads at the same place in aligned instructions name RAM globals.
    def loads(rom: Rom, start: int, size: int) -> dict[int, int]:
        out = {}
        for i in range(0, min(size, 2 * MAX_FUNCTION) & ~1, 2):
            h = rom.half(start + i)
            if h >> 11 == 0x09:
                out[i // 2] = rom.word(((start + i + 4) & ~3) + (h & 0xFF) * 4)
        return out

    globals_: dict[str, dict] = {}
    for a, b in pairs.items():
        sa, sb = rotk_size(a), ttt_size(b)
        mine, theirs = loads(rotk, a, sa), loads(ttt, b, sb)
        if not any(w in data for w in mine.values()):
            continue
        sm = difflib.SequenceMatcher(None, shapes(rotk, a, sa), shapes(ttt, b, sb), autojunk=False)
        for block in sm.get_matching_blocks():
            for k in range(block.size):
                word, other = mine.get(block.a + k), theirs.get(block.b + k)
                if word in data and other is not None and other < ROM_BASE:
                    votes = globals_.setdefault(data[word], dict(rotk=word, votes={}))["votes"]
                    votes[other] = votes.get(other, 0) + 1
    for symbol, entry in globals_.items():
        best = max(entry["votes"].items(), key=lambda kv: kv[1])
        entry["ttt"], entry["agree"], entry["total"] = best[0], best[1], sum(entry["votes"].values())
        entry["votes"] = {hex(k): v for k, v in entry["votes"].items()}

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(functions=matched, globals=globals_), indent=1))
    print(f"ROTK functions {len(functions)}; matched by bytes {direct}, aligned {aligned}, "
          f"through calls {len(matched) - direct - aligned}")
    print(f"RAM globals named {len(globals_)}")
    for symbol in sorted(globals_, key=lambda s: globals_[s]['rotk'])[:40]:
        g = globals_[symbol]
        print(f"  {symbol:32} rotk {g['rotk']:08x} ttt {g['ttt']:08x}  ({g['agree']}/{g['total']})")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
