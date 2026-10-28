"""Run an isolated, muted checkpoint and retain coverage instead of assuming it is native."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

# Executable, game config and save file name per recompiled ROM (tools/lab.py TARGETS).
TARGETS = {
    "stock": ("build/host/ROTK-Recompiled", ".private/game.toml", "save.eeprom"),
    "expanded": ("build/host-expanded/ROTK-Recompiled-Expanded", ".private/expanded/game.toml", "save.sram"),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=900)
    parser.add_argument("--name", default="boot")
    parser.add_argument("--target", choices=sorted(TARGETS), default="stock")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--dummy-window", action="store_true", help="Exercise paced desktop video/audio using silent SDL dummy devices")
    parser.add_argument("--mods-state", type=Path, help="Copy the executable/packages into this run and use an isolated feature-state fixture")
    parser.add_argument("--load-state", type=Path, help="Start a dummy-window check from a copied checkpoint")
    parser.add_argument("--executable", type=Path, help="Use an experimental build of the selected target")
    parser.add_argument("--no-state", action="store_true", help="Skip canonical state export when isolating shutdown problems")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--strict", action="store_true")
    mode.add_argument("--interpreter", action="store_true")
    args = parser.parse_args()
    if args.frames < 1 or not re.fullmatch(r"[A-Za-z0-9_-]+", args.name):
        parser.error("Use positive frames and an alphanumeric checkpoint name")
    if args.load_state and not args.dummy_window:
        parser.error("--load-state currently requires --dummy-window (relative presented-frame count)")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    run = ROOT / ".private/runs" / f"{args.name}-{stamp}"
    run.mkdir(parents=True)
    exe_path, game_config, save_name = TARGETS[args.target]
    exe = ROOT / (exe_path + ".exe")
    if not exe.exists():
        exe = ROOT / exe_path
    if args.executable:
        exe = args.executable.resolve()
    if args.mods_state:
        source = exe.parent
        isolated = run / "bin"
        isolated.mkdir()
        shutil.copy2(exe, isolated / exe.name)
        for dll in source.glob("*.dll"):
            shutil.copy2(dll, isolated / dll.name)
        shutil.copytree(source / "mods/packages", isolated / "mods/packages")
        shutil.copyfile(args.mods_state, isolated / "mods/state.toml")
        exe = isolated / exe.name
    command = [str(exe), "--config", str(ROOT / game_config),
        "--window" if args.dummy_window else "--no-window", "--frames", str(args.frames), "--volume", "0",
        "--save-path", str(run / save_name), "--state-dir", str(run / "states"),
        "--dump-png", str(run / "frame.png")]
    if not args.no_state:
        command += ["--dump-state", str(run / "final.gbas")]
    if args.load_state:
        shutil.copyfile(args.load_state, run / "input.gbas")
        command += ["--load-state", str(run / "input.gbas")]
    env = dict(os.environ)
    for key in ("GBARECOMP_STRICT_STATIC", "GBARECOMP_FORCE_INTERP", "GBARECOMP_INPUT_REPLAY"):
        env.pop(key, None)
    env["GBARECOMP_COVERAGE_JSON"] = str(run / "coverage.json")
    env["GBARECOMP_HEAL_CACHE"] = str(run / "recomp_cache")
    if args.dummy_window:
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")
        env["GBARECOMP_FRAME_PHASE"] = str(run / "frame-phase.csv")
        env["GBARECOMP_DIAGNOSTICS"] = "0"
    if args.strict:
        env["GBARECOMP_STRICT_STATIC"] = "1"
    if args.interpreter:
        env["GBARECOMP_FORCE_INTERP"] = "1"
    if args.input:
        trace = args.input.resolve()
        (run / "input.csv").write_bytes(trace.read_bytes())
        env["GBARECOMP_INPUT_REPLAY"] = str(run / "input.csv")
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    timeout = False
    with (run / "runtime.log").open("w", encoding="utf-8") as log:
        try:
            result = subprocess.run(command, cwd=run, env=env, stdout=log,
                stderr=subprocess.STDOUT, timeout=args.timeout, startupinfo=startup)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = None
            timeout = True
    text = (run / "runtime.log").read_text(errors="replace")
    final = re.search(r"ppu_frames=(\d+)", text)
    if args.dummy_window:
        final = re.search(r"frames_presented=(\d+)", text)
    coverage = json.loads((run / "coverage.json").read_text()) if (run / "coverage.json").exists() else None
    finished = code == 0 and final is not None and int(final[1]) == args.frames
    image = run / "frame.png"
    report = {
        "checkpoint": args.name, "target": args.target, "requested_frames": args.frames,
        "finished": finished, "exit_code": code, "timeout": timeout,
        "dummy_window": args.dummy_window,
        "mode": "interpreter" if args.interpreter else "strict-aot" if args.strict else "aot-with-recovery",
        "coverage": coverage,
        "frame_sha256": hashlib.sha256(image.read_bytes()).hexdigest() if image.exists() else None,
        "command": command,
    }
    (run / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"run": str(run), "finished": finished, "mode": report["mode"],
        "coverage": coverage, "frame_sha256": report["frame_sha256"]}, indent=2))
    if not finished:
        print(text[-2500:], file=sys.stderr)
        raise SystemExit(1)
    if args.strict and (coverage is None or coverage.get("coverage") != "FULLY_STATIC"):
        print("Checkpoint finished but strict native coverage was not established", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
