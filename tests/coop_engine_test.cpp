// Exercise the actual executable's engine hooks without booting a ROM.
#define main rotk_coop_test_game_main
#include "../src/main.cpp"
#undef main
#include "runtime_bus_bridge.h"

int main()
{
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    gba_mod_register_function_entry_plugin(kHeapHookId, kMemoryAddHeap, 1, pcHeapInit);
    gba_mod_register_function_entry_plugin(kPoolHookId, kPoolBuildFreeList, 1, pcActorPools);
    gba_mod_register_function_entry_plugin(kSortHookId, kRenderSort, 0, pcRenderSort);
    gba_mod_disable_all_function_hooks();
    g_layout = PcLayout{};
    gba::g_rom_read32_override = nullptr;
    gba_mod_set_function_hooks_suspended(1);
    gba::GbaBus first, second;
    rotk::pcFeaturesStartCoop();
    for (auto* bus : {&first, &second}) {
        gbarecomp::set_active_bus(bus);
        rotk::pcFeaturesSelectConsole(bus == &first ? 0 : 1);
        ArmCpuState cpu{};
        cpu.R[1] = kHeapArena;
        cpu.R[14] = 0x08010001;
        check(gba_mod_function_entry(kMemoryAddHeap, 1, &cpu) != 0,
              "co-op heap hook runs with solo hooks disabled and globally suspended");
        check(bus->read32(gba::kHostRamBase + kInfoMagic) == kPcInfoMagic,
              "each peer publishes its own engine info");
        check(bus->read32(kHeaps) >= gba::kHostRamBase, "peer heap is in host RAM");
        cpu.R[1] = kStockMainActors;
        cpu.R[2] = kActorSize;
        check(gba_mod_function_entry(kPoolBuildFreeList, 1, &cpu) != 0,
              "co-op pool hook runs while suspended");
        check(bus->read32(kActorPoolMain) == bus->read32(gba::kHostRamBase + kInfoMainPool),
              "peer actor pool agrees with its advertised info");
        check(gba_mod_function_entry(kRenderSort, 0, &cpu) != 0,
              "co-op sorter runs while suspended");
        bus->write32(layout().heapBase + 0x40, bus == &first ? 0xcafe0001 : 0xcafe0002);
    }
    gbarecomp::set_active_bus(&first);
    check(first.read32(layout().heapBase + 0x40) == 0xcafe0001,
          "peer 2 cannot overwrite peer 1 host memory");
    std::uint32_t redirected = 0;
    check(gba::g_rom_read32_override &&
          gba::g_rom_read32_override(kQueueLiterals[0].romAddress, kQueueLiterals[0].stockValue, &redirected) &&
          redirected >= gba::kHostRamBase, "co-op queue literals use the host layout");
    rotk::pcFeaturesEndCoop();
    check(!layout().heap && !layout().actors && !gba::g_rom_read32_override,
          "leaving co-op restores the stock solo layout and queue reader");
    for (const char* id : kCoopEngineHooks)
        check(!gba_mod_function_hook_enabled(id), "leaving co-op restores the disabled solo hook");
    gba_mod_set_function_hooks_suspended(0);
    gbarecomp::set_active_bus(nullptr);
    if (!failures) std::puts("co-op engine: independent peer memory, paired hooks and solo restoration passed");
    return failures ? 1 : 0;
}
