"""Report useful work and actual present intervals; an FPS average is insufficient."""
import argparse
import csv
import json
import math
from pathlib import Path

PERIOD_US = 1e6 * 280896 / 16777216
WORK = ("guest_us", "render_us", "present_us", "audio_us", "pump_us")


def analyze(rows, warmup=120):
    rows = rows[warmup:]
    if not rows:
        raise ValueError("No samples remain after warm-up")
    intervals = [int(row["interval_us"]) for row in rows]
    if any(value <= 0 for value in intervals):
        raise ValueError("Present intervals must be positive; omit the initial frame")
    ordered = sorted(intervals)
    frames = [int(row["frame"]) for row in rows]
    work = [sum(int(row[key]) for key in WORK) for row in rows]
    return {
        "samples": len(rows), "warmup_frames": warmup,
        "native_period_us": PERIOD_US,
        "mean_present_hz": len(intervals) * 1e6 / sum(intervals),
        "interval_p95_us": ordered[math.ceil(len(ordered) * .95) - 1],
        "interval_p99_us": ordered[math.ceil(len(ordered) * .99) - 1],
        "interval_max_us": max(intervals),
        "long_gap_threshold_us": PERIOD_US * 1.5,
        "long_gaps": sum(value > PERIOD_US * 1.5 for value in intervals),
        "work_over_budget": sum(value > PERIOD_US for value in work),
        "missing_guest_frames": sum(max(0, b - a - 1) for a, b in zip(frames, frames[1:])),
        "nonforward_frames": sum(b <= a for a, b in zip(frames, frames[1:])),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--max-long-gaps", type=int)
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error("Warm-up must not be negative")
    try:
        with args.csv.open(newline="") as source:
            report = analyze(list(csv.DictReader(source)), args.warmup)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))
    if args.max_long_gaps is not None:
        if report["long_gaps"] > args.max_long_gaps or report["missing_guest_frames"] or report["nonforward_frames"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
