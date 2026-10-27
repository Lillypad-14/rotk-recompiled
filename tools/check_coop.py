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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path, help="Recording prefix, without .inputs.txt or -1.sram")
    parser.add_argument("--exe", type=Path, default=ROOT / "build/host-expanded/RotkPcLabExpandedNext.exe")
    parser.add_argument("--mods-state", type=Path, default=ROOT / "build/host-expanded/mods/state.toml")
    parser.add_argument("--frames", type=int, default=6000, help="Frames after both consoles link")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--chunk-cycles", type=int, default=1232, help="0 selects the strict scheduler")
    parser.add_argument("--name", default="recording")
    parser.add_argument("--dump-from", type=int, default=0)
    parser.add_argument("--dump-until", type=int, default=0)
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
        shutil.copyfile(prefix + suffix, run / target)
    solo = run / "solo.csv"
    solo.write_text("# gbarecomp-keyinput-v1\n0,0x3ff\n450,0x3f7\n460,0x3ff\n"
                    "600,0x37f\n610,0x3ff\n640,0x37f\n650,0x3ff\n700,0x3fe\n712,0x3ff\n")
    env = dict(os.environ)
    for name in list(env):
        if name.startswith("ROTK_PC_COOP_TEST_"):
            env.pop(name)
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
               GBARECOMP_DIAGNOSTICS="0", GBARECOMP_INPUT_REPLAY=str(solo),
               ROTK_PC_COOP_TEST_FAST="1", ROTK_PC_COOP_TEST_INPUTS=str(run / "inputs.txt"),
               ROTK_PC_COOP_TEST_EXIT_AFTER=str(args.frames),
               ROTK_PC_COOP_TEST_TRACE=str(run / "trace.txt"),
               GBARECOMP_COVERAGE_JSON=str(run / "coverage.json"),
               GBARECOMP_LOCAL_LINK_CHUNK=str(args.chunk_cycles),
               GBARECOMP_HEAL_CACHE=str(ROOT / "recomp_cache"))
    if args.dump_until:
        env.update(ROTK_PC_COOP_TEST_DUMP=str(run / "iwram.bin"),
                   ROTK_PC_COOP_TEST_DUMP_FROM=str(args.dump_from),
                   ROTK_PC_COOP_TEST_DUMP_UNTIL=str(args.dump_until),
                   ROTK_PC_COOP_TEST_RNG_TRACE=str(run / "rng-calls.bin"),
                   GBARECOMP_LINK_CAPTURE=str(run / "link-payloads.txt"))
    command = [str(exe), "--config", str(ROOT / ".private/expanded/game.toml"), "--window",
               "--save-path", str(run / "session.sram"), "--frames", "1500", "--volume", "0"]
    timed_out = False
    with (run / "runtime.log").open("w", encoding="utf-8") as log:
        try:
            process = subprocess.run(command, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                     timeout=args.timeout,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            code = process.returncode
        except subprocess.TimeoutExpired:
            timed_out, code = True, None
    log_text = (run / "runtime.log").read_text(encoding="utf-8", errors="replace")
    trace = run / "trace.txt"
    last_frame, checksum_flags = 0, []
    if trace.exists():
        for line in trace.read_text().splitlines():
            fields = line.split()
            if len(fields) != 11:
                continue
            last_frame = int(fields[0])
            if any(int(fields[index], 16) & 0x100 for index in (3, 8)):
                checksum_flags.append(last_frame)
    disconnects = re.findall(r"co-op test: frame (\d+) console (\d+) scene (\d+) link ff[0-9a-f]{2}", log_text)
    passed = (code == 0 and last_frame >= args.frames and not disconnects and not checksum_flags
              and "split-screen co-op test exit:" in log_text)
    coverage = run / "coverage.json"
    coverage_data = json.loads(coverage.read_text()) if coverage.exists() else None
    passed = passed and coverage_data is not None
    result = dict(passed=passed, run=str(run), exit_code=code, timeout=timed_out,
                  requested_link_frames=args.frames, last_link_frame=last_frame,
                  chunk_cycles=args.chunk_cycles,
                  disconnects=disconnects, checksum_flag_frames=checksum_flags,
                  executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  coverage=coverage_data)
    (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
