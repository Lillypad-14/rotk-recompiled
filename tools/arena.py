"""Run the mod's test arena sweep in the PC port and print what each skill did.

The arena (rotk `make hack HACKS="@cfg test_arena" PC_EDITION=1 ARENA_HERO=<id>`) boots straight
into a map with one hero and a fixture of targets. With sweep ticks set it casts every active row
from a fresh fixture by itself and prints one ARENA_ROW line per row through mGBA's debug output,
which the PC port writes to stderr ("[gba:debug] ...").

This patches a copy of the arena ROM for a sweep, prepares and builds the `arena` target
(tools/lab.py), runs it headless with The Two Towers ROM (legacy skills need its data) and prints
the rows. A Two Towers hero's legacy rows start at 13; --legacy-level sets every Two Towers skill.

usage: python tools/arena.py --rom <arena .gba> --elf <arena .elf> --decomp <mod checkout>
           [--hero 3] [--ticks 90] [--fixture 0] [--legacy-level 5] [--first-row 13]
           [--two-towers <TTT ROM>] [--frames 20000] [--skip-build | --reconfigure]

--reconfigure changes only the sweep settings of the arena already prepared (they are ROM data,
so the recompiled code stays): it patches the prepared ROM, updates its hash and rebuilds the host,
a minute instead of a full prepare.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROM_BASE = 0x08000000
CONFIG_SYMBOL = "testArena_config"
# Config words, in TestArenaConfig order (rotk hacks/patches/test_arena/test_arena.h).
CONFIG_WORDS = 8
# ARENA_ROW values, in TestArenaSweepValue order.
ROW_FIELDS = ("row", "accepted", "recast", "hits", "kills", "damage", "player_hp", "lag_ticks",
              "worst_vblanks", "heap_room", "alloc_failures", "invalid_frees", "bss_guard",
              "hero_state_misuse", "effects_after_clear", "player_downed")
TWO_TOWERS_ROM = "Lord of the Rings, The - The Two Towers (USA, Europe) (En,Fr,De,Es,It,Nl).gba"


def config_offset(elf: Path, nm: str) -> int:
    symbols = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == CONFIG_SYMBOL:
            return int(parts[0], 16) - ROM_BASE
    raise SystemExit(f"{CONFIG_SYMBOL} not found in {elf}: is it an arena build?")


def patched_rom(args: argparse.Namespace) -> Path:
    rom = bytearray(args.rom.read_bytes())
    return write_config(args, rom, ROOT / ".private/arena-sweep.gba")


def write_config(args: argparse.Namespace, rom: bytearray, out: Path) -> Path:
    offset = config_offset(args.elf, args.nm)
    words = struct.unpack_from(f"<{CONFIG_WORDS}I", rom, offset)
    if words[1:] != (0,) * (CONFIG_WORDS - 1) and out.parent != ROOT / ".private/arena":
        raise SystemExit("the arena ROM's config is not the build's default; pass the unpatched ROM")
    struct.pack_into(f"<{CONFIG_WORDS}I", rom, offset, args.hero, args.ticks, args.fixture, 0, 0, 0,
                     args.legacy_level, args.first_row)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(rom))
    # Same build as the ELF: keep its time, so lab.py's ELF-older-than-ROM check holds.
    stat = args.rom.stat()
    os.utime(out, (stat.st_atime, stat.st_mtime))
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", type=Path, required=True, help="the arena ROM (unpatched)")
    parser.add_argument("--elf", type=Path, required=True, help="the arena ELF built with it")
    parser.add_argument("--decomp", type=Path, required=True, help="the mod checkout the arena was built from")
    parser.add_argument("--hero", type=int, default=3, help="HeroId (3 Gandalf)")
    parser.add_argument("--ticks", type=int, default=90, help="ticks each cast runs")
    parser.add_argument("--fixture", type=int, default=0, help="0 dummy, 1 three archers, 2 fire archer")
    parser.add_argument("--legacy-level", type=int, default=5)
    parser.add_argument("--first-row", type=int, default=13)
    parser.add_argument("--two-towers", type=Path, default=ROOT.parent / TWO_TOWERS_ROM)
    parser.add_argument("--frames", type=int, default=20000)
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--skip-build", action="store_true", help="run the arena build as it is")
    parser.add_argument("--reconfigure", action="store_true",
                        help="new sweep settings for the prepared arena: patch its ROM, rebuild the host")
    args = parser.parse_args()

    if args.reconfigure:
        private = ROOT / ".private/arena"
        rom = bytearray((private / "rotk.gba").read_bytes())
        write_config(args, rom, private / "rotk.gba")
        sha1, crc32 = hashlib.sha1(rom).hexdigest(), f"0x{zlib.crc32(rom) & 0xFFFFFFFF:08x}"
        game = (private / "game.toml").read_text()
        game = re.sub(r'(\[rom\][^\[]*?sha1 = ")[0-9a-f]+', lambda m: m.group(1) + sha1, game, flags=re.S)
        game = re.sub(r'crc32 = "0x[0-9a-f]+"', f'crc32 = "{crc32}"', game)
        (private / "game.toml").write_text(game)
        provenance = json.loads((private / "provenance.json").read_text())
        provenance.update(rom_sha1=sha1, rom_crc32=crc32)
        (private / "provenance.json").write_text(json.dumps(provenance, indent=2))
        subprocess.run([sys.executable, str(ROOT / "tools/lab.py"), "build", "--target", "arena",
                        "--decomp", str(args.decomp)], check=True)
    elif not args.skip_build:
        rom = patched_rom(args)
        for command in ("prepare", "build"):
            subprocess.run([sys.executable, str(ROOT / "tools/lab.py"), command, "--target", "arena",
                            "--decomp", str(args.decomp), "--rom", str(rom), "--elf", str(args.elf)],
                           check=True)
    exe = ROOT / "build/host-arena/RotkArena.exe"
    env = dict(os.environ, ROTK_PC_TWO_TOWERS_ROM=str(args.two_towers), SDL_AUDIODRIVER="dummy")
    run = subprocess.run([str(exe), "--config", str(ROOT / ".private/arena/game.toml"), "--no-window",
                          "--frames", str(args.frames), "--volume", "0",
                          "--save-path", str(ROOT / ".private/arena/arena.sram")],
                         capture_output=True, text=True, env=env, cwd=exe.parent)
    lines = [line.split("] ", 1)[1] for line in (run.stdout + run.stderr).splitlines()
             if line.startswith("[gba:debug] ")]
    rows = [line.split()[1:] for line in lines if line.startswith("ARENA_ROW")]
    print(f"{'row':>4} {'cast':>5} {'again':>5} {'hits':>5} {'kills':>5} {'damage':>7} {'hp':>5}")
    for values in rows:
        v = dict(zip(ROW_FIELDS, (int(x) for x in values)))
        print(f"{v['row']:>4} {v['accepted']:>5} {v['recast']:>5} {v['hits']:>5} {v['kills']:>5} "
              f"{v['damage']:>7} {v['player_hp']:>5}")
    done = [line for line in lines if line.startswith("ARENA_DONE")]
    print(done[0] if done else f"no ARENA_DONE in {args.frames} frames ({len(lines)} debug lines)")
    if not lines:
        print("\n".join((run.stderr or "").splitlines()[-15:]))


if __name__ == "__main__":
    main()
