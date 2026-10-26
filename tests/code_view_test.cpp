#include <array>
#include <cstdio>
#include <cstring>
#include <chrono>
#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

int main() {
    gba::GbaBus first, second;
    int failures = 0;
    for (auto* bus : {&first, &second}) {
        gbarecomp::set_active_bus(bus);
        for (unsigned i = 0; i < 0x40000; ++i) bus->ewram_ptr()[i] = i * 13 + (bus == &first ? 7 : 9);
        for (unsigned i = 0; i < 0x8000; ++i) bus->iwram_ptr()[i] = i * 17 + (bus == &first ? 5 : 3);
        for (auto address : {0x02000000u, 0x02040001u, 0x0203fffeu, 0x02fffffeu,
                             0x03000000u, 0x03018000u, 0x03007fffu, 0x03ffffffu,
                             0x04000000u, 0x08000000u, 0x12000000u}) {
            for (unsigned size : {0, 1, 2, 4, 16, 64, 65}) {
                const auto* actual = g_runtime_ram_code_view(address, size);
                std::array<unsigned char, 65> copy{};
                if (actual) std::memcpy(copy.data(), actual, size);
                const auto* expected = bus->native_code_view(address, size);
                if (bool(actual) != bool(expected) || (expected && std::memcmp(copy.data(), expected, size)))
                    ++failures;
            }
        }
        bus->write32(0x03000000, 0xe3a0002a);
        if (!runtime_native_ram_instruction_matches(0x03000000, 0xe3a0002a, 4)) ++failures;
        bus->write32(0x03000000, 0xe3a00063);
        if (runtime_native_ram_instruction_matches(0x03000000, 0xe3a0002a, 4)) ++failures;
    }
    second.write32(0x03000000, 0xe3a00063);
    if (!runtime_native_ram_instruction_matches(0x03000000, 0x63, 2)) ++failures;
    if (runtime_native_ram_instruction_matches(0x03000000, 0xe3a00063, 2)) ++failures;
    if (!runtime_native_ram_instruction_matches_masked(0x03000000, 0xe3a00063, 2, 0xffff)) ++failures;
    // Compare the optimized guard with the established observational view,
    // including mirror aliases and instructions crossing the RAM wrap.
    for (auto* bus : {&first, &second, &first}) {
        gbarecomp::set_active_bus(bus);
        for (uint32_t address : {0x02000000u, 0x0203fffeu, 0x02fffffeu,
                                 0x03000000u, 0x03007ffeu, 0x03fffffeu,
                                 0x04000000u, 0x12000000u}) {
            for (uint32_t size : {2u, 4u}) {
                const auto* bytes = bus->native_code_view(address, size);
                uint32_t expected = 0;
                if (bytes) for (uint32_t i = 0; i < size; ++i) expected |= uint32_t(bytes[i]) << (i * 8);
                if (runtime_native_ram_instruction_matches(address, expected, size) != bool(bytes)) ++failures;
                if (bytes && runtime_native_ram_instruction_matches(address, expected ^ 1u, size)) ++failures;
            }
        }
    }
    first.write32(0x03000000, 0x12345678);
    second.write32(0x03000000, 0x87654321);
    for (auto* bus : {&first, &second, &first}) {
        gbarecomp::set_active_bus(bus);
        if (!runtime_native_ram_instruction_matches(0x03008000, bus->read32(0x03000000), 4)) ++failures;
        bus->write32(0x03008000, bus->read32(0x03000000) ^ 0xffu);
        if (!runtime_native_ram_instruction_matches(0x03000000, bus->read32(0x03008000), 4)) ++failures;
    }
    // A caller replacing the callback owns its read semantics, even in IWRAM.
    const auto normal_view = g_runtime_ram_code_view;
    g_runtime_ram_code_view = +[](uint32_t, uint32_t) -> const uint8_t* {
        static const uint8_t bytes[] = {0x11, 0x22, 0x33, 0x44};
        return bytes;
    };
    if (!runtime_native_ram_instruction_matches(0x03000000, 0x44332211, 4)) ++failures;
    g_runtime_ram_code_view = nullptr;
    if (runtime_native_ram_instruction_matches(0x03000000, 0x44332211, 4)) ++failures;
    g_runtime_ram_code_view = normal_view;
    gbarecomp::set_active_bus(&second);
    second.write32(0x03000000, 0xe3a00063);
    if (!runtime_native_ram_instruction_matches_masked(0x03000000, 0xe3a0002a, 4, 0xffffff00) ||
        runtime_native_ram_instruction_matches_masked(0x03000000, 0xe3a10063, 4, 0xffffff00)) ++failures;
    if (runtime_native_ram_instruction_matches(0x03000000, 0, 0) ||
        runtime_native_ram_instruction_matches(0x03000000, 0, 3)) ++failures;
    // Measure the direct and callback routes back to back in one process;
    // this is a microbenchmark, not an FPS acceptance threshold.
    for (bool direct : {true, false}) {
        gbarecomp::set_active_bus(&second);
        if (!direct) runtime_bind_native_ram_guard_views(nullptr, nullptr);
        const auto start = std::chrono::steady_clock::now();
        unsigned matches = 0;
        for (unsigned i = 0; i < 2000000; ++i)
            matches += runtime_native_ram_instruction_matches(0x03000000, i & 1 ? 0xe3a00063 : 0xe3a0002a, 4);
        if (matches != 1000000) ++failures;
        std::printf("2M live opcode guards (%s): %.3f ms\n", direct ? "direct" : "callback",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    gbarecomp::set_active_bus(nullptr);
    if (g_runtime_ram_code_view(0x03000000, 4)) ++failures;
    std::puts(failures ? "code view FAILED" : "live code views, mirrors, boundaries, writes and rebinding passed");
    return failures ? 1 : 0;
}
