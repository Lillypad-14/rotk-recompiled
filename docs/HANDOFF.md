# Handoff (2026-10-26)

Goal: a PC-exclusive ROTK (vanilla and the Expanded mod's PC edition) with the GBA limits removed.
The GBA edition of the mod is frozen; newer mod versions are PC-only. Read README.md, STATUS.md and
docs/ENGINE_LIMITS.md first.

## Done and verified

- Host RAM 64 MiB at 0x12000000 (`patches/engine-limits.patch`), DMA keeps host addresses, BIOS
  copy/decompress accept them. `RotkHostRamCheck` passes.
- CPU overclock (`GBARECOMP_CPU_OVERCLOCK`, launchers default x4). Lag benefit not measured yet.
- Game heap in host RAM via a `[[mod_function_hook]]` on `memory_addHeap` (0x08032E74), catches stock
  `memory_init` and the mod's `modRam_init`. Vanilla frame hashes unchanged.
- Expanded target (`lab.py --target expanded --decomp ..\rotk`): boots through save format, mod title,
  cutscene, gameplay. All ROM code native (`config/rotk-expanded.extra.toml`).
- Repo: private github.com/Lillypad-14/rotk-recompiled. Submodules: `external/gbarecomp`, `game` (`pc-edition`).

## In progress: actor caps (uncommitted in this repo and in ../rotk)

- `src/main.cpp`: host RAM layout = info block "RPC1" (0x12000000) | 3 per-actor queues | main and
  overlay actor pools | heap. Hook on `pool_buildFreeList` 0x080304E0 (declared in `config/rotk.toml`
  as extra_func + mod_function_hook) builds 1024/160-slot pools (`ROTK_PC_ACTORS`,
  `ROTK_PC_OVERLAY_ACTORS`) and repoints `g_ActorPoolMain`/`g_ActorPoolOverlay`. `g_rom_read32_override`
  repoints the 10 ROM literals of `g_CollisionCheckQueue` (0x03002430), `g_SpriteWorldRenderQueue`
  (0x03002830) and `g_SpritePostCommitQueue` (0x030015E8), all fixed 256-entry IWRAM arrays filled
  without bounds checks.
- Vanilla checked: 7,500-frame first-map frame hash identical to stock (`4580a193...`); state holds the
  RPC1 block and pools. The 900-frame boot hash differs only in the scrolling menu background (the host
  pool build takes no guest cycles, so boot runs slightly ahead).
- Mod side (../rotk, uncommitted): `hacks/lib/pc_host.h` (reads the RPC1 block on `__arm__` builds only)
  and `hacks/lib/actor_ref.c` uses the real capacities. Without this, actors above slot 256 lose their
  references. Mod rebuilt, 96 host tests pass.
- Expanded checked with the updated mod and 1024/160 pools: 8,800-frame route finishes, frame hash
  identical to the earlier Expanded run (`beefab5f...`), 21 misses (IWRAM only, as before). Tip: run
  `lab.py` with `*> log` redirection, piping its stderr to Select-String in PowerShell reports a false
  failure.

## Done 2026-10-26: per-actor buffer audit, render sort, hook return leak

- Audit: the three queues are the only per-actor buffers. The IWRAM sort (0x03002C30, copied from
  0x0800E208) and collision code (0x03002D30, from 0x0800CD5C) are copied by CpuSet through the ROM
  literal override, so they already use the host queues. Sprite cell cache is per graphic (15 entries),
  stock code never turns actor pointers into indices. `g_SpriteFixedRenderQueue` (5 entries) holds
  only ALWAYS_VISIBLE actors (HUD) and does not grow with the actor count.
- The sort read the queue count with `ldrb`: 256+ queued actors sorted only `count & 0xFF`. Now a host
  hook (`pcRenderSort`, same Shell sort, gaps 21/7/3/1, same keys, 32-bit count). Needed a
  `[[code_copy]]` span for the hook site. `ROTK_PC_RENDER_SORT=0` turns it off.
- Framework bug found by it: a handled mod hook skipped the body's `bx lr`, so the caller's pushed
  call-return entry leaked once per hooked call (Expanded aborted with "call-return stack overflow").
  `patches/hook-return.patch` makes the generated guards call `runtime_call_should_return` like the
  skipped return would. The pool hook leaked too, just rarely.
- Checks: stock first-map 7,500 frames `4580a193...` with 1024/160 pools and with 256/40; Expanded
  8,800 frames `beefab5f...` with the sort on and off; 0 call-return evictions everywhere; framework
  codegen tests pass. Collision code stays O(n^2) (native ARM translation), watch it with big crowds.

## Stress test (2026-10-26)

- `ROTK_PC_STRESS_SPAWN=N` (host, info block v2 field `stressSpawn`) makes the mod's debug lab
  (`hacks/patches/debug_cheats/debug_lab.c`, `tickStress`) spawn N orc archers on a grid around the
  player, 16 per frame, and refill the player's HP from then on. Inert on a GBA.
- Expanded route with 400: no crash over 10,000 frames, render queue peak 260..291 (past the old byte
  cap), no call-return evictions. Frame 5,150 draws a dense crowd correctly.
- From about frame 5,300 the opening mission script moves the camera to a scripted scene and waits
  there, since the extra enemies block its condition. That is a test artefact, not an engine fault.
  Do not read the later frozen screenshots as a renderer bug.
- Not measured yet: in-fight frame cost with the crowd (whole-run guest fps averages are meaningless).
- Gotcha: `--tcp` starts the exe paused and the input replay desyncs (ends on the save migration
  screen). Use `check.py --frames N` dumps for scripted routes.

## Sprite pieces past 128 OAM entries (2026-10-26)

- `patches/extra-objects.patch` (framework): `gba::g_extra_obj_provider` hands the PPU extra OAM-format
  entries drawn as indices 128+ in the native, expanded and OBJ-window passes (hardware affine
  matrices). Priority keys now use stride 4096 (BG at +4092) instead of 256/+128, same order.
  `g_dma_observer` reports every DMA (src, dst, bytes).
- Host (`src/main.cpp`, "Extra sprite pieces"): hooks on `sprite_pushOam`, `sprite_hideOamEntry`,
  `sprite_endFrame` keep pieces past OAM in pages per shadow buffer (g_OamShadowA/B) and parity
  half. The DMA observer selects the page that matches the half the VBlank handler copied to OAM.
  `ROTK_PC_EXTRA_OBJECTS` (default 896, 0 = off). `sprite_composeOam` never checks the count itself.
- Equivalence test: `ROTK_PC_OAM_LIMIT=8` routes everything past 8 pieces through the extra path. Stock
  first-map `4580a193...`, Expanded `beefab5f...` and the 400-orc stress frame 5,150 `756a11d9...`
  all match the unlimited runs (up to 128 extra pieces in use).
- The stress crowd needed at most 141 pieces in the observed frames; no queued actor lacked OBJ tiles.
  Still GBA-sized: OBJ tile VRAM (32 KiB, `g_ObjTileBitmap`, 10-bit tile numbers) and 32 affine
  matrices. Unused attr3 halfword of extra entries could carry a VRAM bank later.
- Splitscreen (user asked, 2026-10-26): feasible via two linked GbaInstances in one process (framework
  netplay already runs both seats). Host services here use process-wide state (`layout()`,
  `extraObjects()`, the providers); make them per-instance when that work starts.

## Next

1. OBJ tile VRAM beyond 32 KiB (allocator + PPU bank) when a scene runs out; measure first.
2. Widescreen: the framework already has expanded-view margins and per-game OBJ/BG providers
   (`gba_ppu.h`, docs/WIDESCREEN_STEPC_PLAN.md in the framework).
3. A cleaner stress scenario (bonus map), frame-cost measurement, co-op link audit for u8 actor indices.
4. Host saves, audio (IWRAM mixer is the remaining interpreted code), 60 fps.
5. Licence for this repository's own code (still open). Repo: private github.com/Lillypad-14/rotk-recompiled;
   the `game` submodule tracks the mod repo's `pc-edition` branch (the GBA edition stays on `expanded`).

Rule of thumb: every engine change goes in this repo (host code or framework patch) and is checked on
both targets; regenerate `patches/engine-limits.patch` after framework edits (see git history of this
file's sibling tools for the procedure: diff the framework against pinned revision + audio patch).
