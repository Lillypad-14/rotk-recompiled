# Removing GBA limits

The lab's goal is to take ROTK past the GBA's memory, CPU and rendering limits. Each change lives in a
framework patch under `patches/` (applied in order by `tools/lab.py`) and keeps the original behaviour
reachable, so the faithful build stays available for comparison.

## Host RAM (`patches/engine-limits.patch`)

- Flat, unmirrored RAM at `0x12000000..0x1FFFFFFF`, address space the GBA leaves unmapped. Stock code
  never touches it on its own, so the stock route is unchanged (the 900-frame boot frame hash matches
  the earlier baseline, `88142e65...`).
- Why `0x12000000` and not `0x10000000`: the BIOS copy and decompress calls (CpuSet, CpuFastSet, LZ77,
  RL, Huffman, ...) refuse a source whose `addr & 0x0E000000` is zero. The base keeps bit 25 set, so
  heap data can be decompressed and copied with the BIOS.
- DMA keeps host RAM addresses whole (`gba::dma_address`). Every other DMA address keeps the GBA's
  28-bit mask.
- Backed size: 64 MiB by default. `GBARECOMP_HOST_RAM_MB=N` (1..224) changes it before boot.
- Zero wait states. Reads past the backed size return 0 and writes are dropped (logged with
  `GBARECOMP_LOG_UNMAPPED`).
- Saved states, netplay state and state hashes carry only the written prefix (`host_ram_used`), so an
  unused region costs nothing. A state that needs more host RAM than configured loads without it and
  prints a warning.
- The DMA fast path, native code views (code can run from host RAM) and the debug region list include it.
- Who uses it: the game heap (below). Next, a PC flavour of the mod puts its BSS and larger actor pools
  there.
- Check: `cmake --build build/host --target RotkHostRamCheck` then `build/host/RotkHostRamCheck.exe`.

## Game heap in host RAM (`src/main.cpp`, `config/rotk.toml`)

- Stock `memory_init` (0x08032BA0) registers heap 0 over the EWRAM tail, about 246 KiB, and everything
  dynamic comes out of it: actor pools, sprite frames, mission data. A reviewed `[[mod_function_hook]]`
  on `memory_init` makes the host register heap 0 over host RAM instead (`g_Heaps` 0x03002048,
  `g_HeapCount` 0x03002058, same block layout as `memory_addHeap`).
- `ROTK_PC_HEAP_MB` sets the size (default 32, must fit in host RAM). `ROTK_PC_HEAP_MB=0` keeps the
  stock EWRAM heap.
- `g_Heaps` has a single slot (the next word is `g_HeapCount`), so there is no second heap: heap 0
  moves as a whole. The old EWRAM arena is left unused.
- Observed: the 900-frame boot and the 7,500-frame first-map route give the same frame hashes as the
  stock heap. The first-map state holds about 250 KB of host RAM (the heap's high-water mark) and
  `g_Heaps` points at `0x12000010`.
- The extra heap is only useful once something allocates more: larger actor pools, more effects, and the
  mod's own allocations.

## CPU overclock (`patches/engine-limits.patch`)

- `GBARECOMP_CPU_OVERCLOCK=N` (1, 2, 4, 8, 16) charges guest instructions 1/N of their GBA cycle cost.
  The game gets N times the work per frame. PPU, timers, audio, DMA and halt/idle time keep real GBA
  timing, so music and frame rate are unchanged and only lag frames go away.
- `Play.cmd` defaults to x4. `Play.ps1 -CpuOverclock 1` plays at original speed.
- Linked multiplayer sessions keep native timing. The fractional cycle remainder is not saved in states
  (at most N-1 cycles of drift after a load).
- Observed: the 7,500-frame first-map route reaches the same gameplay scene at x1 and x4. Not yet measured:
  lag frames in a busy fight (needs a fight route or the Expanded ROM's arena).

## Next

1. Expanded ROM as a lab target (symbols from its linked ELF, its own identity and saves). Its
   `mod_ram` calls `memory_addHeap` itself, so the PC flavour sets the heap there.
2. Larger actor pools (256 main, 40 overlay), then the mod's BSS in host RAM.
3. Host renderer: draw from game state instead of 128 OAM slots and 96 KiB VRAM.
