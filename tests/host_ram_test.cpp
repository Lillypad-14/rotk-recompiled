// Host RAM region checks: flat unmirrored storage at 0x12000000, bounded by
// GBARECOMP_HOST_RAM_MB, saved as its written prefix, and invisible to stock
// GBA regions.

#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "snapshot.h"

namespace {
int failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
}  // namespace

int main()
{
    using namespace gba;

    expect(classify(kHostRamBase) == Region::HostRam, "base is host RAM");
    expect(classify(0x1FFFFFFFu) == Region::HostRam, "0x1FFFFFFF is host RAM");
    expect(classify(0x10000000u) == Region::OpenBus, "0x10000000 stays unmapped");
    expect(classify(0x20000000u) == Region::OpenBus, "0x20000000 stays unmapped");
    expect(classify(0x0FFFFFFFu) == Region::Save, "save region unchanged");
    expect((kHostRamBase & 0x0E000000u) != 0, "BIOS copy source check accepts host RAM");
    expect(dma_address(kHostRamBase + 8u) == kHostRamBase + 8u, "DMA keeps host RAM addresses");
    expect(dma_address(0x12000000u - 4u) == 0x01FFFFFCu, "DMA masks other addresses to 28 bits");
    expect(dma_address(0x22000000u) == 0x02000000u, "DMA masks past the window");

    GbaBus bus;
    expect(bus.host_ram_size() == kHostRamDefaultSize, "default host RAM size");
    expect(bus.host_ram_used() == 0, "fresh host RAM is untouched");

    bus.write32(kHostRamBase, 0xDEADBEEFu);
    expect(bus.read32(kHostRamBase) == 0xDEADBEEFu, "32-bit round trip");
    expect(bus.read16(kHostRamBase + 2u) == 0xDEADu, "16-bit view of the same word");
    expect(bus.read8(kHostRamBase + 1u) == 0xBEu, "8-bit view of the same word");

    // No mirroring: the far end of the backed range is distinct storage.
    const uint32_t last = kHostRamBase + static_cast<uint32_t>(kHostRamDefaultSize) - 4u;
    bus.write32(last, 0x12345678u);
    expect(bus.read32(last) == 0x12345678u, "last backed word");
    expect(bus.read32(kHostRamBase) == 0xDEADBEEFu, "no wrap onto the first word");
    expect(bus.host_ram_used() == kHostRamDefaultSize, "written prefix tracks the high end");

    // Past the backed size reads zero and writes are dropped.
    bus.write32(last + 4u, 0xFFFFFFFFu);
    expect(bus.read32(last + 4u) == 0u, "beyond the backed size is unmapped");

    // EWRAM keeps its 256 KiB mirror and never aliases host RAM.
    bus.write32(0x02000000u, 0xCAFEF00Du);
    expect(bus.read32(0x02040000u) == 0xCAFEF00Du, "EWRAM mirror unchanged");
    expect(bus.read32(kHostRamBase) == 0xDEADBEEFu, "EWRAM write leaves host RAM alone");

    // DMA fast path between EWRAM and host RAM.
    expect(bus.dma_copy_ram(0x02000000u, kHostRamBase + 0x100u, 4u), "DMA EWRAM to host RAM");
    expect(bus.read32(kHostRamBase + 0x100u) == 0xCAFEF00Du, "DMA landed in host RAM");

    // Snapshot round trip restores the written prefix and clears the rest.
    GbaBus small;
    small.write32(kHostRamBase + 0x10u, 0x0BADCAFEu);
    gbarecomp::debug::SnapshotWriter writer;
    small.serialize(writer);
    gbarecomp::debug::SnapshotReader reader(writer.buffer().data(), writer.buffer().size());
    bus.deserialize(reader);
    expect(bus.host_ram_used() == 0x14u, "restored written prefix");
    expect(bus.read32(kHostRamBase + 0x10u) == 0x0BADCAFEu, "restored host RAM word");
    expect(bus.read32(kHostRamBase) == 0u, "bytes from before the load are cleared");
    expect(bus.read32(last) == 0u, "high bytes from before the load are cleared");

    if (failures == 0) std::puts("host RAM checks passed");
    return failures == 0 ? 0 : 1;
}
