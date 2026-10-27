# Split-screen co-op

Launcher feature "Split-screen co-op (experimental)" (Gameplay, on by default), `src/local_coop.cpp`.
The game's own two-player link co-op, with both consoles on this PC.

## How it plays

1. Solo play is the normal game with every PC feature.
2. Choosing Multiplayer on the title menu freezes the solo console and opens a local link session:
   two fresh consoles boot unshown from the solo console's save (about 5 s of game time, run
   unpaced), and both are steered into Multiplayer. The window then fades in both screens.
3. Player 1 keeps the normal controls. Player 2 uses a second controller, or WASD, J = A, K = B,
   Space = Start, G = Select, U/I = L/R. The sound is console 1's with console 2's sound effects
   mixed in: console 2 boots with the game's own music option off (cleared in its save image,
   checksum kept valid), so the music is not doubled. Player 2's save keeps that option off.
4. When console 1 is back on the main menu, the session ends: the solo console takes console 1's
   save through its own cartridge writes (the battery flush saves it), B cancels its link prompt,
   and the window returns to one screen. The save is backed up once per run to
   `<save>.before-link` first.

Widescreen works in co-op: both consoles get the window's width (284 by default), with world
margins and the widened actor visibility, and menus pillarboxed. Each console has its own
widescreen state (`widescreenSelectConsole`, switched by the session's bind hook), and its hooks
keep running while the others are suspended. HUD at the edges works on both screens when its
option is on.

Co-op keeps the game's own camera clamp. The game links two simulations of the same level, and each
console tracks both players' cameras; widescreen's clamp only re-clamps the console's own player's
camera, so near a level edge the consoles disagreed on the cameras, ticked different actors and
stopped accepting each other's messages: the game reports a lost connection after 300 frames
without one (2026-10-26, a scripted session dropped at linked frame 6,418 with widescreen alone, and
held 12,000 frames without the clamp). At a level edge the margins in co-op show the area past the
edge. Game logic hooks must give the same result on both consoles.

Smooth motion, real transparency and the extra sprite pages also work in co-op, each with its own
state per console (`pcFeaturesSelectConsole` in src/main.cpp). Smooth motion rarely has an
in-between frame there (about 65 of 4,500 frames per console in the scripted fight): it needs the
next tick composed by the repeated vblank, which mostly happens with the CPU boost, and link
sessions keep native timing.

The engine limits (larger heap, actor pools, render sort, the queue literals and the PC info block)
run on the co-op consoles too: their hooks write through the active bus into that console's own host
RAM. Each console publishes its own PC info block. Suspending these hooks caused a recorded player
session (17:49, 2026-10-26) had console 1 send a packet whose checksum did not match (console 2's
link code at 0x03007570 sets g_MpMessageMask bit 0x100 and stops, and the game reports a lost
connection 300 frames later) at linked frame 3,686, on the local and the strict lockstep scheduler
alike and with every presentation feature off. With them, that session holds through 6,000 frames, the earlier 17:24 session through 12,000,
and the 15-minute random fight script through 54,000 (at most 56 frames between messages).
`GBARECOMP_LOCAL_LINK_CHUNK=<cycles>` sets the chunk length for tests (0 = strict lockstep).

Off during co-op: HD icons, HD audio and the CPU boost.

## Player 2's save

Option "Player 2 save" (`player2-save`):

- "Player 2's own save" (default): console 2 plays on `rotk.player2.sram` next to the normal save.
  The first session makes it as a copy of player 1's; after each session console 2's save is
  written back to it (atomically). Deleting the file starts it over from player 1's again. A file
  that can't be read or is larger than the cartridge's save is left alone, and player 2 plays that
  session on a copy.
- "A copy of yours each session": console 2 loads player 1's save and its writes are discarded.

## Framework pieces (gbarecomp, local sessions only; netplay is unchanged)

- Local link play (`RunOptions::local_link`, runtime.cpp `run_local_link`): the windowed runner
  polls `wanted()` each presented frame and runs the session in its window, with the game's
  `frame()` hook choosing each console's buttons and the screen count.
- Local chunks end a console's turn by throwing (as single-player does), not by returning through
  generated code (`set_local_chunks` turns return yields off). Generated SWI calls do not check for
  a returned suspension, so a turn that ended inside a BIOS call let the caller run on with a stale
  resume PC: a recorded player session crashed on a Thumb dispatch to the SWI vector at linked
  frame 10,192, and half-restored registers can silently diverge the consoles (the "lost connection
  after killing an enemy" reports). With throwing, the same session runs 18,500 frames with at most
  57 frames between link messages. Co-op fights cost 8.6 ms per frame instead of 6.2 (median,
  quiet machine), still inside the 16.7 ms budget.
- Local chunk scheduler (`GbaMultiplayerSession::set_local_chunks`): each console runs a chunk
  (1,232 cycles, one scanline) with its devices ticked inline as in single-player, instead of
  lockstep slices of at most 63 cycles with a per-instruction batching check. IRQs run to completion
  (`g_runtime_sync_irq`). About 3.5x faster; menu frames matched the lockstep scheduler pixel for
  pixel.
- Interpreter fallback (`runtime_set_session_interpreter_fallback`): self-modifying RAM code (the
  sound mixer) runs through the interpreter, as in single-player.
- Loose cable timing (`GbaLinkHub::set_loose_timing`): the consoles' clocks differ by up to about
  15,000 cycles (one runs a chunk, then the other catches up, and IRQ handlers run past a chunk's
  end), so multiplayer transfers keep one timeline. A transfer the primary starts at cycle S takes
  each console's send data and clears its receive registers when that console's own clock reaches
  S, and completes on it at S + duration once every console's data is in; transfers queue per
  console. The first version took a lagging console's data early and cleared its received data
  before it was read (27 times in a 3-minute scripted fight). `GBARECOMP_LINK_TRACE=1` reports
  these hazards when the session ends. The strict (netplay) cable is unchanged.
- Machine pause (`set_machine_paused`): a paused console's clock stops and it resumes without
  catching up.
- Shared view (`LocalLinkPlay::share_view`): both consoles render at the window's margins, and
  `extended_view_frame` runs at each console's frame start
  (`runtime_set_session_frame_start_hook`). `LocalLinkPlay::bind` reports the active console
  (`GbaMultiplayerSession::set_bind_observer`), and `gba_mod_set_function_hook_runs_suspended` keeps
  chosen hooks running in the session.
- Partner audio (`LocalLinkFrame::partner_audio`): console 1's samples are added to console 0's.
- Partner save (`LocalLinkPlay::partner_keeps_save`): console 1's own save file,
  `<stem>.player2<ext>` beside the normal save.
- `gba_mod_set_function_hooks_suspended`, `HostWindow::player_two_buttons` and
  `fit_window_to_surface`.

## Measurements (2026-10-26)

Two consoles, per frame. Fights: `RotkSplitScreenSpike` with the in-game settings (local chunks of
1,232 cycles, session interpreter fallback), saves at the first level, 3 minutes of both players
walking and attacking orcs (frames 5401-10800), medians of 600-frame windows.

| Case | Before the bridge fixes | After |
|---|---|---|
| Quiet machine, co-op fights | 8.1 ms median, p95 11.6, max 13.5, none over 16.7 | 6.2 ms median, p95 8.4, max 12.5, none over |
| Quiet machine, linked menus and level start | 6.7 ms median | 5.4 ms median |
| FF14 running, co-op fights (two interleaved pairs) | 18.7 / 19.3 ms median, about 60% over | 13.9 / 13.4 ms median, 21-32% over |

The bridge fixes (gbarecomp `runtime_arm_default_aborts.cpp`): the interpreter bridge looked for a
static entry after every interpreted instruction (`runtime_has_static_entry`, whose
`lookup_native_ram` was 18% of co-op fight samples, because misses are not cached) and now looks
only after a control transfer; and it no longer copies the CPU state into `g_cpu` at the top of
an instruction when it was just loaded from it (about 3% more). Co-op screenshots every 600
frames are byte-identical before and after. Single-player 1,800-frame crowd-action checkpoint:
same final image (`dba12c45...`), snapshot equal except the RTC byte that also differs between
runs of one build; interpreted instructions 243,569 -> 243,591 (code that falls through into a
static entry now stays interpreted until its next branch).

Earlier, with other games running: the lockstep scheduler on menus took 13 ms mean (20-27% over
budget); local chunks on menus 3.8 ms mean on a quiet machine.

## Testing

- `RotkSplitScreenSpike` (tests/split_screen_spike.cpp): headless runs with per-seat input scripts
  and side-by-side screenshots, and `play` mode (`PlaySplitScreenTest.cmd`).
- Every in-game session is recorded next to the save: `<save>.coop-<time>-1.sram` and `-2.sram`
  (both consoles' saves at the start) and `<save>.coop-<time>.inputs.txt` (both players' buttons).
  Replay one with the first as the save, the second as `rotk.player2.sram`, the inputs as
  `ROTK_PC_COOP_TEST_INPUTS`, the player's mods state and a solo input script that picks
  Multiplayer.
- Link checks: with `ROTK_PC_COOP_TEST_INPUTS`, each console's scene and `g_MpLinkState` changes are
  logged (`ff00` once the game dropped the link), `ROTK_PC_COOP_TEST_FAST=1` runs unpaced,
  `ROTK_PC_COOP_TEST_SHOW_FROM=N` shows (and frame-dumps) from linked frame N, and
  `ROTK_PC_COOP_TEST_TRACE=<file>` writes per frame each console's scene, frames since a link
  message arrived (the game gives up past 300), `g_MpMessageMask`, IE and SIOCNT. A 15-minute random
  fight with the player's own settings (2026-10-26) held the link with at most 57 frames between
  messages. The player's own disconnect came from a build that ran no PC features in co-op; the
  same script on the previous cable code with every feature off also held, with no cleared unread
  data, so that disconnect is not reproduced. The cable hazards the fix removes are its likely
  cause.
- In game, `ROTK_PC_COOP_TEST_INPUTS=<file>` plays both players from `frame p1 p2` lines counted
  from the link. Widescreen check (2026-10-26): saves from `.private/split-screen/`, the solo
  console scripted into Multiplayer, both players through the menus into the first level and 30 s
  of fighting; 284-wide screens with seamless margins, `ROTK_PC_TRACE_MARGINS` silent except the
  34-frame level-load fade at camera 0,0 on each console, 7.1 ms mean per frame.
- Co-op fight timing: from `.private/expanded/play`, `GBARECOMP_SESSION_INTERPRETER=1
  SPLIT_LOCAL_CHUNK=1232 SPLIT_SAVE_P1=../../split-screen/p1.sram SPLIT_SAVE_P2=../../split-screen/p2.sram
  RotkSplitScreenSpike ../rotk.gba ../gba_bios.bin 10800 config/scenarios/split-screen-fight.txt OUT 600`.
  The summary prints mean, median, p95 and max per 600 frames; with other programs running, compare
  medians of interleaved runs.
- In game: `ROTK_PC_COOP_TEST_EXIT_AFTER=N` ends a session N frames after linking. With
  `GBARECOMP_FRAMEDUMP_DIR` the session's frames are saved as `link_<frame>.png`.
- Player 2's save (2026-10-26): a blank save with the scripted Multiplayer route
  (`GBARECOMP_INPUT_REPLAY`) made `rotk.player2.sram`; a marker written into its unused tail came
  back after a second session, and player 1's save stayed without it.
- `RotkPcLabExpandedNext` builds the game under another name while the normal one is running.

## Open

1. More PC features inside co-op: HD icons (needs the high-resolution renderer per console), the
   CPU boost.
2. The audio mixer interpreted on both consoles remains a large cost (interpreter step, state
   copies); native RAM templates for it (roadmap section 2) would help single-player too.
3. Level-edge widescreen margins inside co-op.

## Recorded disconnect regression (2026-10-26)

The normal executable used by the 18:13 recording was still the 17:46 build. Replaying that recording on the archived executable reproduces a checksum-error flag at linked frame 3,286 and link abort at 3,586. The current engine-hook configuration passes both the 17:49 and 18:13 recordings through 6,000 linked frames with no checksum-error flags or aborts.

Stock solo limits are a separate configuration regression: with all optional PC enhancements explicitly disabled, the 17:49 recording still fails on the selected-layout-only implementation. Co-op now constructs a private layout with at least a 4 MiB heap, 1,024 main actors, 160 overlay actors and matching queues/sorter. The engine hooks are enabled as well as allowed during suspension. Each peer writes into its own Host RAM. The prior solo layout, ROM-read callback and hook enable flags are restored on exit. Larger selected sizes are retained. This session layout does not change the solo feature selections.

`RotkCoopEngineCheck` exercises the real registered callbacks while hooks are globally suspended, verifies independent peer info/pools/heap bytes and queue redirects, and checks stock solo restoration. Framework link and multiplayer session tests also pass. The stock-solo-options recording passes 6,000 linked frames after the private-layout change. These checks do not guarantee every campaign, save or future encounter, and execution still uses interpreter recovery (`NOT_STATIC`).

Replay without touching original saves:

```powershell
cmake --build build/host-expanded --target RotkCoopEngineCheck --parallel 2
./build/host-expanded/RotkCoopEngineCheck.exe
python tools/check_coop.py .private/expanded/play/rotk.sram.coop-YYYYMMDD-HHMMSS --exe build/host-expanded/RotkPcLabExpanded.exe --frames 6000 --name regression
```

The prefix requires `.inputs.txt`, `-1.sram` and `-2.sram` sidecars. The runner copies them and the selected executable/packages into `.private/coop-checks`, mutes dummy audio/video, saves a per-frame trace and retains CPU coverage. A pass requires reaching the requested linked frame, an explicit planned test exit, no checksum-error flag, no aborted peer and a coverage report. `--mods-state` selects an isolated options fixture; `--chunk-cycles 0` selects the strict scheduler. Input snapshots and replays remain private.

Final launcher-build verification: the rebuilt normal `RotkPcLabExpanded.exe` passed the 18:13 recording through 6,000 linked frames, with no checksum flags or disconnects. Its SHA-256 is `95ec5799857f3b073457a614e7c49964e12890e6c60150b51cc9e5bf2ed868a1`; the executable installed under `build/host-expanded` matches the tested copy exactly.

## Collision order desync (18:47 recording, 2026-10-26)

The 18:47 recording dropped the link at linked frame 5,866 with the engine limits on. Player 2 drew
one extra gameplay random number after a fight: at frame 5,522 two overlapping enemies (0x2E3,0xF3
and 0x2E5,0xF5) were stunned the other way round on the two consoles.

`actor_checkCollisions` (ROM 0x0800CD5C, run by the Expanded mod from IWRAM 0x03002D30) first moves
the collisionPassive actors (flag bit 17 at +0xE8) of `g_CollisionCheckQueue` to its end by
swapping, which does not keep the queue's order, then tests pairs in order; of two overlapping
enemies the earlier one pushes and the later one is stunned (state 0x0E). Each console also queues
a few objects of its own view, so the swaps left the shared enemies in a different order on each
console. The stun, then the random stream and the packet checksum, diverged.

Fix: in co-op, the hook `rotk.pc.coop-collision-order` (an entry hook at 0x03002D30, declared with
its `[[code_copy]]` in `config/rotk-expanded.extra.toml`) partitions the queue stably first (active,
then passive, each in their original order), so the game's swap pass changes nothing and both
consoles test the shared actors in the same order. It runs while hooks are suspended and is off
outside co-op, so single-player is unchanged. Ruled out on the way: the scheduler (the strict one
fails the same way), widescreen, smooth motion, transparency, extra sprites and the cable.

Any engine pass that orders work by a structure holding console-local objects (effects, the own
view's sprites) can desync the same way; check for it first when a recording diverges in a fight.
