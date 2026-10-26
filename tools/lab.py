"""Prepare and build a private, reproducible ROTK recompilation experiment."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "framework.lock.json").read_text())

# Framework patches, applied in order on top of the pinned revision.
FRAMEWORK_PATCHES = [
    ROOT / "patches/desktop-audio-rate.patch",
    ROOT / "patches/engine-limits.patch",
    ROOT / "patches/hook-return.patch",
    ROOT / "patches/extra-objects.patch",
    ROOT / "patches/mod-state-defaults.patch",
    ROOT / "patches/stereo-audio.patch",
    ROOT / "patches/smooth-present.patch",
    ROOT / "patches/runtime-compiler.patch",
]

# recomp-ui (launcher) patches, applied the same way to external/recomp-ui.
RECOMP_UI = ROOT / "external/recomp-ui"
RECOMP_UI_PATCHES = [
    ROOT / "patches/recomp-ui-gba-audio.patch",
]

STOCK_ROM_SIZE = 0x1000000
EXPANDED_ROM_SIZE = 0x2000000


@dataclass(frozen=True)
class Target:
    """One recompiled ROM: the stock cartridge or the Expanded mod build."""

    name: str
    title: str
    private: Path
    generated: Path
    host_build: Path
    executable: str
    save_type: str
    debug_port: int


TARGETS = {
    "stock": Target("stock", "ROTK PC Lab", ROOT / ".private", ROOT / "build/generated",
                    ROOT / "build/host", "RotkPcLab", "eeprom", 19852),
    "expanded": Target("expanded", "ROTK PC Lab Expanded", ROOT / ".private/expanded",
                       ROOT / "build/expanded/generated", ROOT / "build/host-expanded",
                       "RotkPcLabExpanded", "sram", 19853),
}


def checked(command: list[str | Path], **kwargs: object) -> None:
    print("+", subprocess.list2cmdline([str(x) for x in command]), flush=True)
    subprocess.run([str(x) for x in command], check=True, **kwargs)


def verify_image(path: Path, sha1: str, size: int) -> None:
    if path.stat().st_size != size or hashlib.sha1(path.read_bytes()).hexdigest() != sha1:
        raise ValueError(f"Wrong image revision: {path}")


def apply_framework_patch(framework: Path) -> None:
    apply_patch_stack(framework, FRAMEWORK_PATCHES)
    if RECOMP_UI.exists():
        apply_patch_stack(RECOMP_UI, RECOMP_UI_PATCHES)


def apply_patch_stack(checkout: Path, patches: list[Path]) -> None:
    command = ["git", "-C", str(checkout), "apply"]
    # The patches form an ordered stack and later ones can change an earlier one's context, so
    # check the top first: when it is applied, the whole stack is.
    top = subprocess.run(command + ["--reverse", "--check", str(patches[-1])],
                         capture_output=True)
    if top.returncode == 0:
        return
    for patch in patches:
        applied = subprocess.run(command + ["--reverse", "--check", str(patch)],
                                 capture_output=True)
        if applied.returncode == 0:
            continue
        checked(command + ["--check", str(patch)])
        checked(command + [str(patch)])


def framework_patch_hashes() -> dict[str, str]:
    return {patch.name: hashlib.sha256(patch.read_bytes()).hexdigest()
            for patch in FRAMEWORK_PATCHES + RECOMP_UI_PATCHES}


def decomp_symbols(decomp: Path) -> dict[int, tuple[str, str]]:
    symbols: dict[int, tuple[str, str]] = {}
    for line in (decomp / "config/functions.cfg").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            address, mode, name = line.split()[:3]
            if mode not in ("arm", "thumb"):
                raise ValueError(line)
            symbols[int(address, 16)] = (mode, name)
    return symbols


def elf_function_symbols(elf: Path, readelf: str) -> dict[int, tuple[str, str]]:
    """FUNC symbols of the mod's linked ELF. An odd value marks a THUMB function."""
    output = subprocess.check_output([readelf, "-sW", str(elf)], text=True)
    symbols: dict[int, tuple[str, str]] = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[3] != "FUNC" or fields[6] == "UND":
            continue
        value = int(fields[1], 16)
        address = value & ~1
        mode = "thumb" if value & 1 else "arm"
        # Static functions repeat names across files, so every name carries its address.
        symbols[address] = (mode, f"{fields[7]}_{address:08X}")
    return symbols


def write_symbols(path: Path, symbols: dict[int, tuple[str, str]]) -> None:
    path.write_text("".join(f"0x{address:08X}\t{mode}\t{name}\n"
                            for address, (mode, name) in sorted(symbols.items())))


def expanded_config(target: Target, sha1: str) -> Path:
    """The stock config retargeted at the 32 MiB Expanded image, plus reviewed extra roots."""
    text = (ROOT / "config/rotk.toml").read_text()
    replacements = [
        (r'(?m)^name = ".*"$', 'name = "The Return of the King: Expanded (mod build)"'),
        (r'(?m)^id = ".*"$', 'id = "rotk_expanded"'),
        (r"(?m)^size = 0x[0-9A-Fa-f]+$", f"size = 0x{EXPANDED_ROM_SIZE:08X}"),
        (r'(?m)^sha1 = ".*"$', f'sha1 = "{sha1}"'),
    ]
    for pattern, replacement in replacements:
        text, count = re.subn(pattern, replacement, text, count=1)
        if count != 1:
            raise ValueError(f"config/rotk.toml has no line for {pattern}")
    extra = ROOT / "config/rotk-expanded.extra.toml"
    if extra.exists():
        text += "\n# config/rotk-expanded.extra.toml\n" + extra.read_text()
    path = target.private / "rotk-expanded.toml"
    path.write_text(text)
    return path


def prepare(args: argparse.Namespace) -> None:
    target = TARGETS[args.target]
    revision = subprocess.check_output(
        ["git", "-C", str(args.framework), "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != LOCK["revision"]:
        raise ValueError(f"Framework revision {revision} differs from the pinned revision")
    apply_framework_patch(args.framework)
    verify_image(args.bios, LOCK["bios_sha1"], 0x4000)
    target.private.mkdir(parents=True, exist_ok=True)
    symbols = decomp_symbols(args.decomp)
    if target.name == "stock":
        rom = args.rom or args.decomp / "rotk.gba"
        verify_image(rom, LOCK["baseline_rom_sha1"], STOCK_ROM_SIZE)
        rom_bytes = rom.read_bytes()
        config = ROOT / "config/rotk.toml"
    else:
        rom = args.rom or args.decomp / "build/rotk_hack.gba"
        rom_bytes = rom.read_bytes()
        if len(rom_bytes) != EXPANDED_ROM_SIZE:
            raise ValueError(f"Expanded ROM must be {EXPANDED_ROM_SIZE:#x} bytes: {rom}")
        elf = args.elf or args.decomp / "build/hack.elf"
        if elf.stat().st_mtime < rom.stat().st_mtime - 60:
            raise ValueError(f"{elf} is older than {rom}; rebuild the hack so symbols match")
        mod_symbols = elf_function_symbols(elf, args.readelf)
        symbols.update(mod_symbols)
        print(f"Expanded: {len(mod_symbols)} mod function roots from {elf.name}")
    sha1 = hashlib.sha1(rom_bytes).hexdigest()
    crc32 = zlib.crc32(rom_bytes) & 0xFFFFFFFF
    rom_copy = target.private / "rotk.gba"
    rom_copy.write_bytes(rom_bytes)
    shutil.copyfile(args.bios, target.private / "gba_bios.bin")
    if target.name == "expanded":
        config = expanded_config(target, sha1)
    symbol_file = target.private / "functions.tsv"
    write_symbols(symbol_file, symbols)
    if not (args.framework / "build/CMakeCache.txt").exists():
        checked(["cmake", "-S", args.framework, "-B", args.framework / "build",
                 "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"])
    core = args.framework / "build/gba_recompile.exe"
    if not core.exists():
        core = args.framework / "build/gba_recompile"
    checked(["cmake", "--build", args.framework / "build", "--target", "gba_recompile", "--parallel", str(args.jobs)])
    checked([core, "--rom", rom_copy, "--symbols", symbol_file,
             "--config", config, "--codegen-shards", "16",
             "--out", target.generated / "cart"])
    checked([core, "--bios", target.private / "gba_bios.bin", "--config",
             args.framework / "bios/gba_bios.toml", "--out", target.generated / "bios"])
    game = f'''[game]
name = "{target.title}"
short_name = "rotk"
[bios]
path = "{(target.private / 'gba_bios.bin').as_posix()}"
sha1 = "{LOCK['bios_sha1']}"
[rom]
path = "{rom_copy.as_posix()}"
sha1 = "{sha1}"
crc32 = "0x{crc32:08x}"
[save]
type = "{target.save_type}"
[runtime]
debug_port = {target.debug_port}
window_title = "{target.title}"
'''
    (target.private / "game.toml").write_text(game)
    (target.private / "provenance.json").write_text(json.dumps({
        "target": target.name,
        "framework": revision, "function_roots": len(symbols),
        "framework_patches_sha256": framework_patch_hashes(),
        "decomp_commit": subprocess.check_output(["git", "-C", str(args.decomp), "rev-parse", "HEAD"], text=True).strip(),
        "rom_source": str(rom), "rom_sha1": sha1, "rom_crc32": f"0x{crc32:08x}",
        "bios_sha1": LOCK["bios_sha1"],
    }, indent=2) + "\n")


def build(args: argparse.Namespace) -> None:
    target = TARGETS[args.target]
    apply_framework_patch(args.framework)
    provenance_path = target.private / "provenance.json"
    if not provenance_path.exists():
        raise ValueError(f"Prepare the {target.name} target first: python tools/lab.py prepare --target {target.name}")
    provenance = json.loads(provenance_path.read_text())
    provenance.pop("framework_patch_sha256", None)
    provenance["framework_patches_sha256"] = framework_patch_hashes()
    provenance_path.write_text(json.dumps(provenance, indent=2) + "\n")
    command: list[str | Path] = ["cmake", "-S", ROOT, "-B", target.host_build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        f"-DGBARECOMP_ROOT={args.framework.as_posix()}",
        f"-DROTK_CART_DIR={(target.generated / 'cart').as_posix()}",
        f"-DGBARECOMP_GENERATED_BIOS_DIR={(target.generated / 'bios').as_posix()}",
        f"-DROTK_EXECUTABLE={target.executable}",
        f"-DROTK_GAME_NAME={target.title}",
        f"-DROTK_ROM_SHA1={provenance['rom_sha1']}",
        f"-DROTK_ROM_CRC32={provenance['rom_crc32']}",
        f"-DROTK_DEBUG_PORT={target.debug_port}"]
    if args.sdl2:
        command += [f"-DSDL2_INCLUDE_DIR={(args.sdl2 / 'include/SDL2').as_posix()}",
                    f"-DSDL2_LIBRARY={(args.sdl2 / 'lib/libSDL2.dll.a').as_posix()}"]
    checked(command)
    checked(["cmake", "--build", target.host_build, "--target", target.executable, "--parallel", str(args.jobs)])
    compiler = Path(shutil.which("clang++") or "").parent
    for name in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll"):
        source = compiler / name
        if source.exists():
            shutil.copyfile(source, target.host_build / name)
    if args.sdl2:
        shutil.copyfile(args.sdl2 / "bin/SDL2.dll", target.host_build / "SDL2.dll")


def default_checkout(submodule: str, sibling: str) -> Path:
    """The submodule checkout when present, else the sibling folder of an older workspace layout."""
    inside = ROOT / submodule
    return inside if (inside / ".git").exists() else ROOT.parent / sibling


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "build"))
    parser.add_argument("--target", choices=sorted(TARGETS), default="stock")
    parser.add_argument("--framework", type=Path, default=default_checkout("external/gbarecomp", "gbarecomp"))
    parser.add_argument("--decomp", type=Path, default=default_checkout("game", "rotk"),
                        help="decomp + mod checkout (the game submodule)")
    parser.add_argument("--rom", type=Path, help="stock: rotk.gba, expanded: the mod's rotk_hack.gba")
    parser.add_argument("--elf", type=Path, help="expanded: the mod's linked hack.elf")
    parser.add_argument("--readelf", default=shutil.which("arm-none-eabi-readelf") or str(
        Path.home() / "AppData/Local/rotk-tools/arm-gcc/bin/arm-none-eabi-readelf.exe"))
    parser.add_argument("--bios", type=Path, default=ROOT.parent / "gba_bios.bin")
    legacy_sdl2 = ROOT.parent / "rotk-recomp-work/deps/SDL2-2.32.10/x86_64-w64-mingw32"
    parser.add_argument("--sdl2", type=Path, default=legacy_sdl2 if legacy_sdl2.exists() else None,
                        help="SDL2 SDK root with include/SDL2, lib and bin (else CMake finds SDL2)")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        (prepare if args.command == "prepare" else build)(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
