"""Disassemble Two Towers code with ROTK names.

Calls and literal-pool words are named from the matcher's .private/ttt/symbols.json
(tools/ttt_match.py), so Two Towers functions read like the ROTK ones they mirror. The ROM's
first 0x60000 bytes are disassembled once with llvm-objdump into .private/ttt/code.s.

usage: python tools/ttt_dis.py ADDRESS [LENGTH]    (hex; LENGTH defaults to 0x100)
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIVATE = ROOT / ".private/ttt"
ROM_PATH = ROOT.parent / "Lord of the Rings, The - The Two Towers (USA, Europe) (En,Fr,De,Es,It,Nl).gba"
ROM_BASE = 0x08000000
CODE_BYTES = 0x60000


def disassembly() -> dict[int, str]:
    listing = PRIVATE / "code.s"
    if not listing.exists():
        binary = PRIVATE / "code.bin"
        obj = PRIVATE / "code.o"
        binary.write_bytes(ROM_PATH.read_bytes()[:CODE_BYTES])
        subprocess.run(["llvm-objcopy", "-I", "binary", "-O", "elf32-littlearm", str(binary), str(obj)], check=True)
        out = subprocess.run(["llvm-objdump", "-D", "-j", ".data", "--triple=thumbv4t-none-eabi",
                              f"--adjust-vma={ROM_BASE:#x}", str(obj)],
                             capture_output=True, text=True, check=True).stdout
        listing.write_text(out)
    lines = {}
    for line in listing.read_text().splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
        if m:
            text = re.sub(r"<_binary[^>]*>", "", m.group(2))
            lines[int(m.group(1), 16)] = re.sub(r"\s+", " ", text).strip()
    return lines


def symbol_names() -> dict[int, str]:
    symbols = json.loads((PRIVATE / "symbols.json").read_text())
    names = {}
    for name, value in symbols.get("functions", {}).items():
        if isinstance(value, dict) and value.get("ttt"):
            names[value["ttt"] & ~1] = name
    return names


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("address", type=lambda v: int(v, 16))
    parser.add_argument("length", type=lambda v: int(v, 16), nargs="?", default=0x100)
    args = parser.parse_args()
    rom = ROM_PATH.read_bytes()
    lines = disassembly()
    names = symbol_names()
    for address in range(args.address, args.address + args.length, 2):
        text = lines.get(address)
        if text is None:
            continue
        note = ""
        if m := re.search(r"\bbl 0x([0-9a-f]+)", text):
            note = names.get(int(m.group(1), 16), "")
        if m := re.search(r"\[pc, #0x[0-9a-f]+\] @ 0x([0-9a-f]+)", text):
            # objdump leaves these comments relative to the section start
            literal = int(m.group(1), 16) % ROM_BASE
            value = struct.unpack_from("<I", rom, literal)[0]
            note = f"={value:#x} {names.get(value & ~1, '')}".rstrip()
        print(f"{address:08x} {text}" + (f"    ; {note}" if note else ""))


if __name__ == "__main__":
    main()
