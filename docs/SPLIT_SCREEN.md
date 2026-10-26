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

The co-op consoles are plain GBAs: every PC feature hook is suspended while the session runs
(mod function hooks, scanline, extra-object, ROM-read and DMA hooks) and the CPU boost does not
apply.

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
- Local chunk scheduler (`GbaMultiplayerSession::set_local_chunks`): each console runs a chunk
  (1,232 cycles, one scanline) with its devices ticked inline as in single-player, instead of
  lockstep slices of at most 63 cycles with a per-instruction batching check. IRQs run to completion
  (`g_runtime_sync_irq`). About 3.5x faster; menu frames matched the lockstep scheduler pixel for
  pixel.
- Interpreter fallback (`runtime_set_session_interpreter_fallback`): self-modifying RAM code (the
  sound mixer) runs through the interpreter, as in single-player.
- Loose cable timing (`GbaLinkHub::set_loose_timing`): a transfer starts on the primary's clock
  and is timed on each console's own clock, so consoles whose clocks differ by a chunk or a pause
  still link. ROTK's handshake, level select and gameplay work with it.
- Machine pause (`set_machine_paused`): a paused console's clock stops and it resumes without
  catching up.
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

1. PC features inside co-op (needs per-console state for each feature).
2. The audio mixer interpreted on both consoles remains a large cost (interpreter step, state
   copies); native RAM templates for it (roadmap section 2) would help single-player too.
3. The framework pieces live only in the `external/gbarecomp` working tree, not in `patches/`.
