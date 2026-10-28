"""Package the Windows release download from the current builds (docs/RELEASING.md).

Copies both executables and the files they load into .private/release/ROTK-Recompiled, stages
GBARecomp's TinyCC for self-heal, adds the licences and PLAY.txt, and zips it. Build both targets
first, with ROTK_GUI_SUBSYSTEM=ON and the edition patch; nothing personal (settings, saves, caches)
is copied.

usage: python tools/make_release.py VERSION    (e.g. v0.1.0)
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "build/host"
HOST_EXPANDED = ROOT / "build/host-expanded"
ENGINE = ROOT / "external/gbarecomp"

FROM_HOST = ["ROTK-Recompiled.exe", "SDL2.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll",
             "expanded.bps", "launcher-window.ini"]
LICENSES = {ROOT / "LICENSE": "ROTK-Recompiled-MIT.txt",
            ROOT / "patches/LICENSE.md": "GBARecomp-PolyForm-Noncommercial.md",
            ROOT / "CREDITS.md": "CREDITS.md",
            ROOT / "release/TinyCC-LGPL-2.1.txt": "TinyCC-LGPL-2.1.txt"}
# fetch_tcc.ps1 stages engine headers that only forward to arm-recomp-core by a relative path, which
# does not exist beside a shipped executable; the real header goes in their place.
FORWARDED_HEADERS = {"runtime_arm_types.h": ENGINE / "external/arm-recomp-core/profiles/armv4t_gba/runtime_arm_types.h"}


def pe_subsystem(path: Path) -> int:
    data = path.read_bytes()[:4096]
    pe = int.from_bytes(data[0x3C:0x40], "little")
    return int.from_bytes(data[pe + 24 + 68:pe + 24 + 70], "little")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version")
    parser.add_argument("--out", type=Path, default=ROOT / ".private/release")
    args = parser.parse_args()

    exes = [HOST / "ROTK-Recompiled.exe", HOST_EXPANDED / "ROTK-Recompiled-Expanded.exe"]
    for exe in exes:
        if pe_subsystem(exe) != 2:
            raise SystemExit(f"{exe} is a console program: configure with -DROTK_GUI_SUBSYSTEM=ON and rebuild")

    folder = args.out / "ROTK-Recompiled"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    for name in FROM_HOST:
        shutil.copy2(HOST / name, folder / name)
    shutil.copy2(exes[1], folder / exes[1].name)
    shutil.copytree(HOST / "assets", folder / "assets")
    shutil.copytree(HOST / "mods/packages", folder / "mods/packages")

    toolchain = folder / "overlay_toolchain"
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(ENGINE / "tools/fetch_tcc.ps1"), "-Toolchain", str(toolchain), "-EngineRoot", str(ENGINE),
                    "-CacheDir", str(args.out / "tcc_cache")], check=True)
    for name, real in FORWARDED_HEADERS.items():
        shutil.copy2(real, toolchain / "include" / name)
    for header in (toolchain / "include").glob("*.h"):
        for line in header.read_text().splitlines():
            if line.lstrip().startswith("#include \"..") or line.lstrip().startswith("#include \"/"):
                raise SystemExit(f"{header.name} still includes outside the bundle: {line.strip()}")

    (folder / "licenses").mkdir()
    for source, name in LICENSES.items():
        shutil.copy2(source, folder / "licenses" / name)
    play = (ROOT / "release/PLAY.txt").read_text().replace("{version}", args.version)
    (folder / "PLAY.txt").write_text(play)

    archive = args.out / f"ROTK-Recompiled-{args.version}-windows"
    zip_path = Path(shutil.make_archive(str(archive), "zip", args.out, "ROTK-Recompiled"))
    print(f"{zip_path} ({zip_path.stat().st_size // 1024} KiB)")


if __name__ == "__main__":
    main()
