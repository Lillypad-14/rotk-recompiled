"""Watch a recorded co-op session in a window, at normal speed with sound.

With no recording given, plays the newest one in .private/expanded/play. Closing the window ends it.
The replay runs on private copies of both starting saves, so your saves are untouched.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path, nargs="?", help="Recording prefix, without .inputs.txt")
    parser.add_argument("--exe", type=Path, default=ROOT / "build/host-expanded/ROTK-Recompiled-Expanded.exe")
    parser.add_argument("--frames", type=int, default=0, help="Linked frames to show (default: the whole recording)")
    args = parser.parse_args()
    recording = args.recording
    if recording is None:
        recordings = sorted((ROOT / ".private/expanded/play").glob("*.coop-*.inputs.txt"))
        if not recordings:
            parser.error("no recordings in .private/expanded/play")
        recording = recordings[-1].with_name(recordings[-1].name[:-len(".inputs.txt")])
    frames = args.frames
    if not frames:
        last = [line.split()[0] for line in Path(str(recording) + ".inputs.txt").read_text().splitlines()
                if line[:1].isdigit()]
        frames = int(last[-1]) + 600 if last else 6000
    print(f"Playing {recording.name} for {frames} linked frames", flush=True)
    command = [sys.executable, str(ROOT / "tools/check_coop.py"), str(recording), "--exe", str(args.exe),
               "--frames", str(frames), "--timeout", str(frames // 30 + 300), "--name", "watch",
               "--visible", "--paced", "--sound"]
    subprocess.run(command, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
