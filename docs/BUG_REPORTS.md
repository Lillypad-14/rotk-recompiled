# Bug reports, replays, save states and rewind

## Bug report hotkey

F12 (config.ini `[KeyMap] BugReport`) copies into `bug-reports/<date>-<time>/` beside the executable,
then opens that folder (`src/bug_report.cpp`):

- this session's diagnostic replay segments and their sidecars (`diagnostics/replays/*-<pid>-*`,
  recorded by the framework in windowed play, see the framework's docs/DIAGNOSTIC_REPLAY.md),
- the save and player 2's save (`<save>.player2.sram`),
- the newest co-op recording (`<save>.coop-<time>.inputs.txt`, `-1.sram`, `-2.sram`),
- `mods/state.toml` and the `.ini` files of the working folder (key bindings, settings),
- a `README.txt` listing them. The ROM and BIOS are never copied.

It works in single-player and in split-screen co-op. A co-op recording replays with
`tools/check_coop.py` or `tools/regress.py`, a diagnostic replay with the framework's
`tools/replay_diagnostic.py`.

Checked (2026-10-27) by calling the report at the end of a scripted co-op run: it copied both saves,
the newest recording and `state.toml`. The hotkey itself has not been pressed in a test.

## Watching a co-op recording

```powershell
python tools/watch_replay.py                     # the newest recording in .private/expanded/play
python tools/watch_replay.py .private/expanded/play/rotk.sram.coop-20261027-152517
```

It plays the whole recording in a window at normal speed with sound, on private copies of both
starting saves (through `tools/check_coop.py --visible --paced --sound`). Closing the window ends it.

## Save states and rewind

The framework's Assist Tools are on (`RunOptions::expose_assist_tools`, `src/main.cpp`): the in-game
menu has save and load state and "Rewind 1 second", and the toggle "Assist Tools" turns them off.

| Action | Keyboard | Controller |
|---|---|---|
| Load state 1-9 | F1-F9 | |
| Save state 1-9 | Shift+F1-F9 | |
| Rewind (1 s per step, repeats while held) | 1 | left trigger |
| Fast-forward (while held) | 2 or Tab | right trigger |

A state is about 1 MB: the GBA's memory plus the PC's game memory (`host_ram_used`, 660 KB in a
first-map fight), which the framework saves with the bus, so the larger engine limits restore too.
Rewind keeps 20 s at four points a second, about 80 MB. Save states and rewind cover single-player;
the co-op session's two consoles are not saved. PC presentation state (smooth motion, health bars)
starts over after a load.
