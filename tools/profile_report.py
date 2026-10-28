"""Name the samples of a co-op profile (tools/check_coop.py --profile) with the executable's symbols.

Prints the functions with the most samples, flat (the function the emulation thread was in).
"""

from __future__ import annotations

import argparse
import bisect
import shutil
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE_BASE = 0x140000000


def symbols(exe: Path, nm: str) -> tuple[list[int], list[str]]:
    output = subprocess.run([nm, "--defined-only", "-C", "-n", str(exe)], capture_output=True, text=True).stdout
    addresses, names = [], []
    for line in output.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addresses.append(int(parts[0], 16))
            names.append(parts[2])
    return addresses, names


def short(name: str, width: int) -> str:
    name = name.replace("(anonymous namespace)::", "")
    name = name.split("(")[0] if "(" in name else name
    return name if len(name) <= width else name[: width - 3] + "..."


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="A tools/check_coop.py run folder (profile.txt and bin/)")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--nm", default=shutil.which("llvm-nm") or "llvm-nm")
    args = parser.parse_args()
    exe = next((args.run / "bin").glob("*.exe"))
    counts, modules, callers = {}, Counter(), []
    for line in (args.run / "profile.txt").read_text().splitlines():
        if line.startswith("#"):
            continue
        if line.startswith("module "):
            name, caller, samples = line[len("module "):].rsplit(" ", 2)
            modules[Path(name).name] += int(samples)
            callers.append((Path(name).name, int(caller, 16), int(samples)))
            continue
        offset, samples = line.split()
        counts[int(offset, 16)] = int(samples)
    outside = sum(modules.values())
    addresses, names = symbols(exe, args.nm)
    functions = Counter()
    for offset, samples in counts.items():
        index = bisect.bisect_right(addresses, IMAGE_BASE + offset) - 1
        functions[names[index] if index >= 0 else "?"] += samples
    total = sum(counts.values()) + outside
    print(f"{total} samples, {100 * outside / max(total, 1):.1f}% in other modules:")
    def function(offset: int) -> str:
        index = bisect.bisect_right(addresses, IMAGE_BASE + offset) - 1
        return names[index] if index >= 0 and offset else "?"
    for name, samples in modules.most_common(8):
        print(f"{100 * samples / total:6.2f}%  {samples:7}  [{name}]")
        via = Counter()
        for module, caller, count in callers:
            if module == name:
                via[function(caller)] += count
        for caller, count in via.most_common(3):
            print(f"{'':17}  from {short(caller, 100)} ({100 * count / samples:.0f}%)")
    for name, samples in functions.most_common(args.top):
        print(f"{100 * samples / total:6.2f}%  {samples:7}  {short(name, 110)}")


if __name__ == "__main__":
    main()
