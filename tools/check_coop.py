"""Replay a recorded co-op session against isolated copies of both starting saves."""

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

ROOT = Path(__file__).resolve().parents[1]
CONSOLE_TRACE = re.compile(r" \d+ -?\d+ ([0-9a-f]{8}) ie=\S+ sio=\S+ rng=(\S+)")
RNG_SPLIT_FRAMES = 30
MAIN_MENU_SCENE = "5"
LINK_LOG = re.compile(r"co-op test: frame (\d+) console (\d+) scene (\d+) link ([0-9a-f]{2})[0-9a-f]{2}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path, help="Recording prefix, without .inputs.txt or -1.sram")
    parser.add_argument("--exe", type=Path, default=ROOT / "build/host-expanded/RotkPcLabExpanded.exe")
    parser.add_argument("--mods-state", type=Path, default=ROOT / "build/host-expanded/mods/state.toml")
    parser.add_argument("--frames", type=int, default=6000, help="Frames after both consoles link")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--chunk-cycles", type=int, default=1232, help="0 selects the strict scheduler")
    parser.add_argument("--name", default="recording")
    parser.add_argument("--config", type=Path, default=ROOT / ".private/expanded/game.toml")
    parser.add_argument("--inputs", type=Path, help="Inputs file in place of the recording's")
    parser.add_argument("--dump-from", type=int, default=0)
    parser.add_argument("--dump-until", type=int, default=0)
    parser.add_argument("--visible", action="store_true", help="Show the window (muted)")
    parser.add_argument("--profile", type=int, nargs=2, metavar=("FROM", "UNTIL"),
                        help="Profile the emulation thread over these linked frames (profile.txt)")
    parser.add_argument("--solo-route", type=Path,
                        help="Keyinput replay (gbarecomp-keyinput-v1) that walks the normal game to Multiplayer")
    parser.add_argument("--solo-frames", type=int, default=1500, help="Frames the normal game may take to get there")
    parser.add_argument("--sound", action="store_true", help="Play the sound (otherwise muted)")
    parser.add_argument("--paced", action="store_true", help="Run at normal speed, for frame times")
    parser.add_argument("--show-from", type=int, default=0, help="Dump the window's frames from this linked frame")
    args = parser.parse_args()
    if args.frames < 1 or args.chunk_cycles < 0 or not re.fullmatch(r"[A-Za-z0-9_-]+", args.name):
        parser.error("Use positive frames and an alphanumeric name")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    run = ROOT / ".private/coop-checks" / f"{args.name}-{stamp}"
    binary = run / "bin"
    binary.mkdir(parents=True)
    source = args.exe.resolve()
    exe = binary / source.name
    shutil.copy2(source, exe)
    for dll in source.parent.glob("*.dll"):
        shutil.copy2(dll, binary / dll.name)
    shutil.copytree(source.parent / "mods/packages", binary / "mods/packages")
    shutil.copyfile(args.mods_state, binary / "mods/state.toml")
    prefix = str(args.recording.resolve())
    for suffix, target in [("-1.sram", "session.sram"), ("-2.sram", "session.player2.sram"),
                           (".inputs.txt", "inputs.txt")]:
        source_file = args.inputs if args.inputs and target == "inputs.txt" else Path(prefix + suffix)
        shutil.copyfile(source_file, run / target)
    solo = run / "solo.csv"
    if args.solo_route:
        shutil.copyfile(args.solo_route, solo)
    else:
        solo.write_text("# gbarecomp-keyinput-v1\n0,0x3ff\n450,0x3f7\n460,0x3ff\n"
                        "600,0x37f\n610,0x3ff\n640,0x37f\n650,0x3ff\n700,0x3fe\n712,0x3ff\n")
    env = dict(os.environ)
    for name in list(env):
        if name.startswith("ROTK_PC_COOP_TEST_"):
            env.pop(name)
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
               GBARECOMP_DIAGNOSTICS="0", GBARECOMP_INPUT_REPLAY=str(solo),
               ROTK_PC_COOP_TEST_INPUTS=str(run / "inputs.txt"),
               ROTK_PC_COOP_TEST_EXIT_AFTER=str(args.frames),
               ROTK_PC_COOP_TEST_TRACE=str(run / "trace.txt"),
               GBARECOMP_COVERAGE_JSON=str(run / "coverage.json"),
               GBARECOMP_LOCAL_LINK_CHUNK=str(args.chunk_cycles),
               GBARECOMP_HEAL_CACHE=str(ROOT / "recomp_cache"))
    if args.visible:
        env.pop("SDL_VIDEODRIVER")
        env.pop("SDL_RENDER_DRIVER")
    if args.sound:
        env.pop("SDL_AUDIODRIVER")
    if args.profile:
        env.update(ROTK_PC_COOP_TEST_PROFILE=str(run / "profile.txt"),
                   ROTK_PC_COOP_TEST_PROFILE_FROM=str(args.profile[0]),
                   ROTK_PC_COOP_TEST_PROFILE_UNTIL=str(args.profile[1]))
    if not args.paced:
        env.update(ROTK_PC_COOP_TEST_FAST="1")
    if args.show_from:
        (run / "frames").mkdir()
        env.update(ROTK_PC_COOP_TEST_SHOW_FROM=str(args.show_from), GBARECOMP_FRAMEDUMP_DIR=str(run / "frames"),
                   GBARECOMP_FRAMEDUMP_START="999999999", GBARECOMP_FRAMEDUMP_COUNT="2000")
    if args.dump_until:
        env.update(ROTK_PC_COOP_TEST_DUMP=str(run / "iwram.bin"),
                   ROTK_PC_COOP_TEST_DUMP_FROM=str(args.dump_from),
                   ROTK_PC_COOP_TEST_DUMP_UNTIL=str(args.dump_until),
                   ROTK_PC_COOP_TEST_RNG_TRACE=str(run / "rng-calls.bin"),
                   GBARECOMP_LINK_CAPTURE=str(run / "link-payloads.txt"),
                   ROTK_PC_COOP_TEST_DUMP_EWRAM_TOP=str(run / "ewram-top.bin"))
    command = [str(exe), "--config", str(args.config.resolve()), "--window",
               "--save-path", str(run / "session.sram"), "--frames", str(args.solo_frames)] + ([] if args.sound else ["--volume", "0"])
    timed_out = False
    with (run / "runtime.log").open("w", encoding="utf-8") as log:
        try:
            process = subprocess.run(command, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                     timeout=args.timeout,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" and not args.visible else 0)
            code = process.returncode
        except subprocess.TimeoutExpired:
            timed_out, code = True, None
    log_text = (run / "runtime.log").read_text(encoding="utf-8", errors="replace")
    session = re.findall(r"local link (session ended after .*|late frames .*)", log_text)
    trace = run / "trace.txt"
    last_frame, checksum_flags = 0, []
    rng_matched, rng_differs, rng_split = 0, None, None
    if trace.exists():
        for line in trace.read_text().splitlines():
            # Per console: scene, frames since a link message, g_MpMessageMask, then rng=cursor/remaining.
            consoles = CONSOLE_TRACE.findall(line)
            if not line[:1].isdigit() or len(consoles) != 2:
                continue
            last_frame = int(line.split(maxsplit=1)[0])
            if any(int(mask, 16) & 0x100 for mask, _ in consoles):
                checksum_flags.append(last_frame)
            # The consoles tick the game on alternate frames at times, so one can be a draw ahead
            # for a frame. Only a difference lasting RNG_SPLIT_FRAMES counts as a split.
            if consoles[0][1] == consoles[1][1]:
                rng_matched, rng_differs = last_frame, None
            elif rng_differs is None:
                rng_differs = last_frame
            if rng_split is None and rng_differs is not None and last_frame - rng_differs >= RNG_SPLIT_FRAMES:
                rng_split = rng_differs
    # The link state's high byte is 0xFF until the session starts and again once a console drops it.
    disconnects, started = [], set()
    for frame, console, scene, high in LINK_LOG.findall(log_text):
        if high != "ff":
            started.add(console)
        elif console in started:
            disconnects.append((frame, console, scene))
    # Both players back on the main menu before the requested frame: the recording ended co-op.
    final_scenes = {console: scene for _, console, scene, _ in LINK_LOG.findall(log_text)}
    left_coop = ("split-screen co-op test exit:" not in log_text and bool(session) and
                 set(final_scenes.values()) == {MAIN_MENU_SCENE})
    passed = (code == 0 and not disconnects and not checksum_flags and rng_split is None and
              (left_coop or (last_frame >= args.frames and "split-screen co-op test exit:" in log_text)))
    coverage = run / "coverage.json"
    coverage_data = json.loads(coverage.read_text()) if coverage.exists() else None
    passed = passed and coverage_data is not None
    result = dict(passed=passed, run=str(run), exit_code=code, timeout=timed_out,
                  requested_link_frames=args.frames, last_link_frame=last_frame,
                  chunk_cycles=args.chunk_cycles,
                  disconnects=disconnects, checksum_flag_frames=checksum_flags,
                  session_report=session,
                  players_left_coop=left_coop,
                  rng_last_matched_frame=rng_matched, rng_split_frame=rng_split,
                  executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  coverage=coverage_data)
    (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
