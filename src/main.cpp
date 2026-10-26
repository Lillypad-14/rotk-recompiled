#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_memory.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace {

// memory_init (stock) and modRam_init (Expanded) register heap 0 over the
// EWRAM tail, about 246 KiB. The PC heap turns that one memory_addHeap call
// into the same heap 0 over host RAM, so every memory_malloc in the game
// (actor pools, sprite frames, mission data) has room to grow. Other
// memory_addHeap calls run as written. ROTK_PC_HEAP_MB=0 keeps the stock heap.
constexpr std::uint32_t kMemoryAddHeap = 0x08032E74u;
constexpr std::uint32_t kHeapArena = 0x02002640u;  // g_HeapArena
constexpr std::uint32_t kHeaps = 0x03002048u;      // Heap g_Heaps[1]
constexpr std::uint32_t kHeapCount = 0x03002058u;  // u32 g_HeapCount
constexpr std::uint32_t kHeapAlign = 16u;          // sizeof(HeapBlock)
constexpr unsigned long kDefaultHeapMb = 32;
constexpr const char* kHeapHookId = "rotk.pc-heap";

unsigned long envMb(const char* name, unsigned long fallback)
{
    const char* value = std::getenv(name);
    return value ? std::strtoul(value, nullptr, 10) : fallback;
}

std::uint32_t heapBytes()
{
    static const std::uint32_t bytes = []() -> std::uint32_t {
        const unsigned long heapMb = envMb("ROTK_PC_HEAP_MB", kDefaultHeapMb);
        const unsigned long hostMb =
            envMb("GBARECOMP_HOST_RAM_MB", gba::kHostRamDefaultSize / (1024u * 1024u));
        if (heapMb == 0) {
            std::fprintf(stderr, "[rotk] stock EWRAM heap\n");
            return 0u;
        }
        if (heapMb > hostMb) {
            std::fprintf(stderr, "[rotk] ROTK_PC_HEAP_MB=%lu exceeds host RAM (%lu MiB); stock heap\n",
                         heapMb, hostMb);
            return 0u;
        }
        std::fprintf(stderr, "[rotk] game heap: %lu MiB of host RAM\n", heapMb);
        return static_cast<std::uint32_t>(heapMb * 1024u * 1024u);
    }();
    return bytes;
}

// memory_addHeap(0, g_HeapArena, end) becomes memory_addHeap(0, kHostRamBase,
// kHostRamBase + size), done on the guest's behalf.
int pcHeapInit(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t size = heapBytes();
    if (size == 0 || cpu->R[0] != 0u || cpu->R[1] != kHeapArena) return 0;
    const std::uint32_t base = gba::kHostRamBase;
    const std::uint32_t end = base + size;
    const std::uint32_t firstBlock = base + kHeapAlign;

    bus_write_u32(kHeaps + 0u, firstBlock);
    bus_write_u32(kHeaps + 4u, firstBlock);
    bus_write_u32(kHeaps + 8u, end - base - kHeapAlign);
    bus_write_u32(kHeaps + 12u, end);
    for (std::uint32_t field = 0; field < kHeapAlign; field += 4u)
        bus_write_u32(firstBlock + field, 0u);
    bus_write_u32(kHeapCount, bus_read_u32(kHeapCount) + 1u);

    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    else cpu->cpsr &= ~0x20u;
    return 1;
}

void enablePcHeap()
{
    gba_mod_set_function_hook_enabled(kHeapHookId, heapBytes() != 0);
}

}  // namespace

int main(int argc, char** argv)
{
    gba_mod_register_function_entry_plugin(kHeapHookId, kMemoryAddHeap, 1, pcHeapInit);
#if defined(GBARECOMP_ENABLE_MODS)
    // Mod activation clears every entry hook before its reset callbacks run.
    gba_mod_register_reset_callback(enablePcHeap);
#endif
    enablePcHeap();

    gbarecomp::RunOptions options;
    options.builtin_game_name = ROTK_GAME_NAME;
    options.builtin_rom_sha1 = ROTK_ROM_SHA1;
    options.builtin_rom_crc32 = ROTK_ROM_CRC32;
    return gbarecomp::run_game(argc, argv, options);
}
