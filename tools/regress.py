"""Replay every recorded co-op session against a build and compare with the previous run.

Each recording goes through tools/check_coop.py. The summary lands in .private/regress/<time>.json,
and a recording that passed in the latest earlier summary but fails now is reported as a regression.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / ".private/regress"


def replay(recording: Path, args: argparse.Namespace) -> dict:
    name = recording.name.split(".coop-")[-1]
    command = [sys.executable, str(ROOT / "tools/check_coop.py"), str(recording), "--frames", str(args.frames),
               "--name", f"regress-{name}", "--exe", str(args.exe), "--timeout", str(args.timeout)]
    output = subprocess.run(command, capture_output=True, text=True).stdout
    try:
        result = json.loads(output[output.index("{"):])
    except ValueError:
        return dict(recording=name, passed=False, error=output[-500:])
    result.pop("coverage", None)
    result["recording"] = name
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/host-expanded/RotkPcLabExpanded.exe")
    parser.add_argument("--recordings", type=Path, default=ROOT / ".private/expanded/play")
    parser.add_argument("--frames", type=int, default=6000, help="Frames after both consoles link")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    recordings = sorted(path.with_name(path.name[:-len(".inputs.txt")])
                        for path in args.recordings.glob("*.coop-*.inputs.txt"))
    if not recordings:
        parser.error(f"no recordings in {args.recordings}")
    previous = {}
    earlier = sorted(RESULTS.glob("*.json"))
    if earlier:
        previous = {r["recording"]: r for r in json.loads(earlier[-1].read_text())["results"]}

    results, regressions = [], []
    print(f"{'recording':18} {'result':6} {'frames':>6} {'drops':>5} {'csum':>4} {'rng split':>9}", flush=True)
    for recording in recordings:
        result = replay(recording, args)
        results.append(result)
        name = result["recording"]
        if not result["passed"] and previous.get(name, {}).get("passed"):
            regressions.append(name)
        print(f"{name:18} {'PASS' if result['passed'] else 'FAIL':6} {result.get('last_link_frame', 0):>6} "
              f"{len(result.get('disconnects', [])):>5} {len(result.get('checksum_flag_frames', [])):>4} "
              f"{str(result.get('rng_split_frame')):>9}", flush=True)

    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    summary = dict(exe=str(args.exe.resolve()), frames=args.frames, results=results, regressions=regressions)
    (RESULTS / f"{stamp}.json").write_text(json.dumps(summary, indent=2) + "\n")
    failed = [r["recording"] for r in results if not r["passed"]]
    print(f"{len(results) - len(failed)}/{len(results)} passed", end="")
    print(f"; regressions since the last run: {', '.join(regressions)}" if regressions else "")
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
