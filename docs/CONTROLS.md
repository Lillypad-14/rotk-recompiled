# Controls overhaul (plan)

Goal: controller and keyboard mapping that needs no explanation, for one player or split-screen co-op,
on a desk or a Steam Deck. Status (2026-10-28): phases 1-4 done; phase 5 (real hardware) to do.

Decisions:

- One Controls screen, shared by the launcher and the in-game menu (same files, press-to-bind in both).
- Nintendo-layout pads use their printed labels: GBA A is the button marked A.
- Each player's input device can be changed at any time, in solo play and in co-op.

## Today

| | Keyboard | Controller |
|---|---|---|
| Player 1 | Rebindable in the launcher (`keybinds.ini` `[player1]`) | Fixed in `host_window.cpp` (`pad_map`), not rebindable |
| Player 2 | 10 choice options (`local-coop` feature `p2-key-*`) | 10 choice options (`p2-pad-*`) |

- Player 2's controls are edited as 20 scroll-through lists, in the launcher's mod options or the
  in-game menu (`src/player2_controls.cpp`). There is no press-to-bind.
- Devices are assigned by connection order: player 1 gets the first controller the window opened,
  player 2 the next one (`HostWindow::player_two_buttons`). There is no way to pick or swap them.
- Button names are GBA names everywhere, whatever pad is held. No conflict warnings, no reset, a
  fixed stick dead zone of 16000.
- recomp-ui already has what the launcher side needs: per-pad profiles keyed by SDL GUID in
  `input.ini` (`common/pad_binds.c`), per-player keyboard binds (`keybinds.ini` `[player1]`,
  `[player2]`), per-player device combos (`##csrc`), press-to-bind and dead zones. The GBA profile
  declares one player (`consoles/gba/gba_profile.h`, `max_players 1`) and no pad spec, so none of it
  shows.

## Design

**Per player: a device and two binding sets.**

- Device: *Keyboard*, *a specific controller* (by GUID, shown by name), or *Any controller* (the
  first free one, today's behaviour and the default). Keyboard binds stay active alongside a
  controller unless both players share the keyboard.
- Keyboard binds: `keybinds.ini` `[player1]` / `[player2]` (existing format).
- Controller binds: `input.ini`, one profile per controller GUID with a global fallback, through
  `pad_binds.c`. Binds follow the controller, not the player, so a pad keeps its layout when it
  moves between players.
- Defaults: GBA A/B on the A/B buttons, L/R on the bumpers, Start/Select on Start/Back, d-pad and
  left stick move. Nintendo pads: `SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS` set explicitly so A is
  the printed A (verify on a Switch Pro, directly and through Steam Input).

**Changing devices at any time.**

- Controls screen: a device picker per player listing what is connected now.
- *Press to assign*: "Player 2: press a button" on the Controls screen; the next device that
  presses becomes player 2's.
- *Swap players* (devices and binds trade places).
- A controller disconnects: that player falls back to the keyboard and a toast says so; the next
  button on any free controller reclaims the seat. Reconnecting the same controller restores it.

**Binding.**

- Select a GBA button, press the key or button. Esc (or holding the button) cancels.
- Binding something another action already uses swaps the two, with a one-line note; nothing is
  ever left unbound by accident.
- Reset per player (keyboard, controller, or both).
- Dead zone slider per controller (stored in its `input.ini` profile).

**Labels.** Controller buttons are shown with the held pad's names (`SDL_GameControllerGetType`):
Xbox A/B/X/Y/LB/RB/View/Menu, PlayStation Cross/Circle/Square/Triangle/L1/R1/Share/Options,
Nintendo B/A/Y/X/L/R/-/+. The co-op start caption uses them too.

**Migration.** The `local-coop` `p2-key-*` / `p2-pad-*` options are copied once into `keybinds.ini`
`[player2]` and the global `input.ini` mapping, then removed from the manifest and the in-game menu.

## Where the code goes

Framework changes stay patches on the pinned revisions (`patches/`, applied by `tools/lab.py`).

- recomp-ui (`recomp-ui-*.patch`): GBA profile to 2 players with a pad spec (10 buttons, d-pad
  cardinals), so the launcher's existing Controls panels appear; button-name tables per pad type.
- gbarecomp (`host_window.cpp`): read `input.ini` profiles; replace the fixed `pad_map` and
  `player_two_buttons` with one per-player resolver (device, then keyboard and pad binds); device
  assignment, swap, hot-plug and press-to-assign; dead zone from the profile.
- Runtime UI (`recomp_runtime_ui.c`): a *binding* item (shows the current key/button, captures the
  next press) and a *device* item, so the in-game Controls page is the same screen as the launcher.
- Game (`src/controls.cpp`, replacing `src/player2_controls.cpp`): the Controls page, toasts,
  migration.

## Phase 1 (done)

`patches/controls.patch` (gbarecomp):

- `src/runtime/player_controls.{h,cpp}`: the files, defaults, assignment, claim, swap, conflict
  swaps and resolution, without SDL. `tests/player_controls` covers them headless
  (`player_controls_tests`).
- `host_window.cpp`: every controller is opened; `reassign_controllers` hands them out on connect,
  disconnect and every change; `player_held` resolves a player's keys and controller. Player 1's
  fixed `pad_map`, `bind_sc` and player 2's separate pad and key tables are gone. Player 1 now also
  moves with the left stick (only player 2 did).
- `runtime.h`: `player_controls`, `set_player_controls`, `connected_controllers`,
  `player_controller`, `player_waiting_for_controller`, `begin_device_claim`, `take_device_notice`
  for the game's menus (phase 3).
- Auto is stored as no `SourceP<n>` line, because today's launcher reads 0..2 only.

Migration (`ControlsVersion` in `config.ini [Controller]`): 1 at the first load sets player 2's keys
to their own defaults (existing `keybinds.ini` files carry the launcher's copy of player 1's keys in
`[player2]`); 2 applies the old `local-coop` options when the game hands them over
(`set_player_two_controls`, at co-op activation), unless the player already changed controls in the
new system. A changed old controller button becomes the shared `[mapping]` entry.

The in-game "Player 2 controls" rows (`src/player2_controls.cpp`) now read and write the new
settings, so they keep working until phase 3 replaces them.

Checked: `player_controls_tests`; an isolated boot (`tools/check.py --mods-state`) writes the three
files as above; an old `p2-key-a = "Q"` / `p2-pad-r = "y"` arrives as `[player2] a = Q` and
`[mapping] r = y`.

## Phase 2 (done)

`patches/recomp-ui-controls.patch`:

- GBA profile: two players and a controller column on the Controller page (`has_pad_binds`). A
  player whose source is a controller edits that controller's layout; Auto and Keyboard edit keys.
- `consoles/gba/gba_pad_binds.c`: the GBA table over the shared `input.ini` store, by the same key
  names the engine reads, default dead zone 50.
- Source 3 = Auto ("Auto (keyboard + any controller)"), the seeded default for both players
  (`launcher_seam.h` in `controls.patch`).
- Reset on player 2's keys gives player 2's WASD layout, not the generic store's copy of player 1's.

The old `p2-key-*` / `p2-pad-*` options stay in the manifest (only declared options keep their saved
values, and the migration reads them) in the group "Old player 2 controls", marked as replaced.
Remove them a release or two later.

Checked: both patches rebuild the working trees from the pinned revisions; a scratch program wrote
bindings through `rui_gba_pad_binds_set` and read them with `controls::load` (and back after an
engine save); the Expanded build boots isolated. The launcher page itself has not been looked at on
screen yet.

## Phase 3 (done)

- In-game menu, section Controls (`src/controls_menu.cpp`, replacing `src/player2_controls.cpp`):
  each player's device (Auto, Keyboard, up to five connected controllers by name, None), "press to
  choose device", Swap players, and pages Player 1/2 keyboard and controller: ten press-to-bind rows
  in pad order, Reset, and the controller's stick dead zone. A controller page edits the player's
  controller, or the shared fallback while they have none (it says which).
- recomp-ui (`recomp-ui-controls.patch`): row type `RECOMP_RUNTIME_UI_BINDING`, a row that shows
  `get_text` and runs `run_action` on accept.
- gbarecomp (`controls.patch`): `begin_binding_capture`; the next key, button or stick/trigger
  direction (past 75%) binds, before the menu sees it. Only the player's own controller counts.
  Escape or 10 s cancels; the prompt stays in the status line while it waits. A used binding swaps.
  Press to choose works the same way. A missing or returning controller shows a 4 s toast.
- `RotkControlsCheck` (`tests/controls_test.cpp`, 46 checks): a headless window (SDL dummy video)
  with SDL virtual controllers covers migration, Auto, the left stick, press to choose, press to
  bind (keys and buttons, conflicts, Escape, another player's controller ignored), unplug and
  replug, swap, the three files, and every kind of Controls menu row through the menu's hooks.

Not checked on screen: the menu's look, and the launcher page (phase 5).

## Phase 4 (done)

- `controls::pad_style` / `display_name`: buttons as the held controller prints them. Xbox (A, B,
  View, Menu, LB, RT), PlayStation (Cross, Circle, Share, Options, L1, R2), Nintendo (A, B, -, +,
  L, ZR) and generic names; the menu's controller pages use the player's controller's style.
- Nintendo pads: SDL already reports face buttons by printed label by default
  (`SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS` = 1), so SDL's A is the button marked A and the default
  GBA A is that button. The host now sets the hint explicitly. Through Steam Input a pad is whatever
  Steam presents (an Xbox pad unless Steam's Nintendo layout option is on).
- Launcher controller column: readable generic names instead of `input.ini` source strings.
- Stick dead zone: per controller, in the menu (phase 3); the launcher's slider stays off for GBA.

## Phases

1. **Engine:** per-player resolver, P1 controller rebindable, `input.ini` and `[player2]` read,
   devices by GUID, swap, hot-plug fallback, migration. Headless test: scripted pad events give the
   expected KEYINPUT for both players.
2. **Launcher:** enable the GBA two-player Controls panels; check they write what phase 1 reads.
3. **In-game Controls page:** binding and device items, press to assign, swap, conflict swap, reset.
4. **Labels and Nintendo layout:** per-type names, explicit label hint, dead zone slider.
5. **Hardware pass:** Xbox, PlayStation, Switch Pro, 8BitDo, Steam Deck (built-in + external pad,
   Steam Input on and off), keyboard-only co-op. Then README and `docs/SPLIT_SCREEN.md` updates.
