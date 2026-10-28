"""Make the launcher's Expanded patch: a BPS from the vanilla ROM to the Expanded ROM the build uses.

Prints the CMake settings for the vanilla build (ROTK_EDITION_PATCH, ROTK_EDITION_SHA1).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLIPS = Path(os.environ.get("LOCALAPPDATA", "")) / "rotk-tools/flips/flips.exe"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vanilla", type=Path, default=ROOT / "game/rotk.gba")
    parser.add_argument("--expanded", type=Path, default=ROOT / ".private/expanded/rotk.gba")
    parser.add_argument("--out", type=Path, default=ROOT / ".private/expanded/expanded.bps")
    parser.add_argument("--flips", default=str(FLIPS) if FLIPS.exists() else shutil.which("flips") or "flips")
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([args.flips, "--create", "--bps-delta", str(args.vanilla), str(args.expanded), str(args.out)],
                   check=True)
    sha1 = hashlib.sha1(args.expanded.read_bytes()).hexdigest()
    print(f"{args.out} ({args.out.stat().st_size} bytes)")
    print(f'-DROTK_EDITION_PATCH="{args.out.resolve().as_posix()}" -DROTK_EDITION_SHA1={sha1}')


if __name__ == "__main__":
    main()
