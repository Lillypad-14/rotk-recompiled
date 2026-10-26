#include <cstdio>
#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
extern "C" uint32_t g_irq_nest_depth;


int main() {
    gba::GbaBus bus;
    auto& io = bus.io();
    io.set_bus(&bus);
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    check(!io.irq_pending(), "reset has no pending enabled IRQ");
    io.write16(gba::IoReg::IE, gba::GbaIo::IrqVBlank | gba::GbaIo::IrqDma2);
    io.request_irq(gba::GbaIo::IrqDma2);
    check(!io.irq_pending(), "IME masks a high-byte pending IRQ");
    io.write16(gba::IoReg::IME, 1);
    check(io.irq_pending(), "enabling IME exposes pending DMA IRQ immediately");
    io.write16(gba::IoReg::IF, gba::GbaIo::IrqVBlank);
    check(io.irq_pending(), "acknowledging an unrelated IRQ preserves DMA IRQ");
    io.write16(gba::IoReg::IF, gba::GbaIo::IrqDma2);
    check(!io.irq_pending(), "acknowledging the DMA IRQ clears eligibility");
    io.request_irq(gba::GbaIo::IrqTimer0);
    check(!io.irq_pending(), "disabled timer IRQ does not become eligible");
    io.request_irq(gba::GbaIo::IrqVBlank);
    check(io.irq_pending(), "low-byte enabled IRQ is observed immediately");
    io.write16(gba::IoReg::IE, 0);
    check(!io.irq_pending(), "IE changes take effect without a device event");
    io.write16(gba::IoReg::IE, gba::GbaIo::IrqVBlank);
    io.write16(gba::IoReg::IME, 0x100);
    check(!io.irq_pending(), "only IME bit zero enables interrupts");

    check(!io.dma_steal_pending(), "no DMA cycles pending initially");
    bus.write32(0x02000000, 0x89abcdef);
    io.write32(0x0d4, 0x02000000); // DMA3 source
    io.write32(0x0d8, 0x02000010); // DMA3 destination
    io.write16(0x0dc, 1);
    io.write16(0x0de, 0x8400); // immediate, word, enabled
    check(bus.read32(0x02000010) == 0x89abcdef, "immediate DMA still copies the word");
    check(io.dma_steal_pending() && io.dma_steal_pending(), "pending probe does not consume stolen cycles");
    check(io.take_dma_steal_cycles() > 0, "the transfer still charges stolen cycles");
    check(!io.dma_steal_pending() && io.take_dma_steal_cycles() == 0, "draining clears the pending cycle debt");
    gbarecomp::set_active_bus(&bus);
    g_cpu.R[15] = 0x08000000;
    g_cpu.cpsr = 0x1f;
    check(!runtime_should_yield(), "ordinary cartridge instruction continues");
    io.write8(gba::IoReg::HALTCNT, 0);
    check(runtime_should_yield(), "HALT is observed immediately");
    io.clear_halt();
    g_runtime_break_pc = g_cpu.R[15];
    check(runtime_should_yield(), "matching debug breakpoint still yields");
    g_runtime_break_pc += 4;
    check(!runtime_should_yield(), "unmatched breakpoint continues");
    g_runtime_break_pc = 0;
    g_runtime_force_interp_hook = +[](uint32_t pc, int thumb) -> int {
        return pc == 0x08000000 && thumb == 0;
    };
    check(runtime_should_yield(), "mutable-code hook can force a yield");
    g_runtime_force_interp_hook = nullptr;
    gbarecomp::RuntimeTimingContext timing;
    gbarecomp::runtime_capture_timing_context(timing);
    ++timing.vblank_starts;
    gbarecomp::runtime_restore_timing_values(timing);
    g_irq_nest_depth = 1;
    check(!runtime_should_yield(), "pending VBlank cannot unwind a nested IRQ");
    g_irq_nest_depth = 0;
    check(runtime_should_yield(), "pending VBlank yields once after IRQ return");
    check(!runtime_should_yield(), "consumed VBlank does not repeatedly yield");
    gbarecomp::set_active_bus(nullptr);
    check(!runtime_should_yield(), "unbound bus has no HALT");
    if (!failures) std::puts("live IRQ, DMA debt, HALT, debug, mutable-code and VBlank yield checks passed");
    return failures ? 1 : 0;
}
