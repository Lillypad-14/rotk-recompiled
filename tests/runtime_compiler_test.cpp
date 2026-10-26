// Compile, load and execute authored ARM instructions, then warm-load the DLL.
// This catches missing tools, incompatible compilers, broken exports and ABI.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "overlay_compile.h"
#include "overlay_abi.h"

namespace {
ArmCpuState cpu{};
unsigned off = 0;
unsigned long long cycles = 0;
std::uint32_t breakPc = 0;
std::uint32_t resumePc = 0;
RuntimeWaitTable waits{};
void (*entryHook)(std::uint32_t) = nullptr;
void tick(std::uint32_t n) { cycles += n; }
int noYield() { return 0; }
int condition(unsigned) { return 1; }
int noMod(std::uint32_t, int, ArmCpuState*) { return 0; }
int returning(std::uint32_t) { return 1; }
void branch(std::uint32_t pc) { cpu.R[15] = pc & ~1u; }
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::absolute(argv[1]) / std::to_string(stamp);
    // mov r0,#42; bx lr
    const std::uint8_t code[] = {0x2a, 0x00, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1};
    gbarecomp::OverlayWorkItem work;
    work.pc = work.base = 0x08000000;
    work.bytes = code;
    work.size = sizeof(code);
    GbaOverlayCallbacks cb{};
    cb.abi_version = GBA_OVERLAY_ABI_VERSION;
    cb.cpu = &cpu;
    cb.runtime_insn_trace = cb.runtime_trace_enabled = &off;
    cb.runtime_cycles = &cycles;
    cb.runtime_break_pc = &breakPc;
    cb.runtime_resume_pc = &resumePc;
    cb.runtime_waits = &waits;
    cb.runtime_fn_entry_hook = &entryHook;
    cb.runtime_tick = tick;
    cb.runtime_should_yield = noYield;
    cb.arm_cond_passes = condition;
    cb.gba_mod_function_entry = noMod;
    cb.runtime_call_should_return = returning;
    cb.runtime_dispatch_with_exchange = branch;
    cb.runtime_dispatch = branch;
    const std::uint8_t thumb_code[] = {0x08, 0x46, 0x70, 0x47}; // mov r0,r1; bx lr
    for (bool thumb : {false, true}) {
      work.thumb = thumb;
      work.bytes = thumb ? thumb_code : code;
      work.size = thumb ? sizeof(thumb_code) : sizeof(code);
      for (bool compile : {true, false}) {
        gbarecomp::OverlayCompiled result;
        std::string error;
        if (!gbarecomp::overlay_compile_one(work, dir.string(), &cb, compile,
                gbarecomp::HealBackend::Gcc, &result, &error)) {
            std::fprintf(stderr, "FAIL: %s\n", error.c_str());
            return 1;
        }
        cpu.R[0] = 0;
        cpu.R[1] = 42;
        cpu.R[14] = 0x08001000;
        cpu.R[15] = work.pc;
        cpu.cpsr = thumb ? 0x3f : 0x1f;
        result.fn();
        if (cpu.R[0] != 42 || !cycles) return 1;
        // Resume at bx lr: the mov must not run a second time, and the
        // resume marker must be consumed through the shared host pointer.
        cpu.R[0] = 99;
        resumePc = work.pc + (thumb ? 2 : 4);
        result.fn();
        if (cpu.R[0] != 99 || resumePc) return 1;
    }
    }
    if (argc == 3) {
        std::ifstream file(argv[2], std::ios::binary);
        std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(file)), {});
        if (image.empty()) return 2;
        work.bytes = image.data();
        work.size = image.size();
        work.thumb = true;
        for (auto pc : {0x08020004u, 0x0801CD24u, 0x08022C84u}) {
            work.pc = pc;
            gbarecomp::OverlayCompiled result;
            std::string error;
            if (!gbarecomp::overlay_compile_one(work, dir.string(), &cb, true,
                    gbarecomp::HealBackend::Gcc, &result, &error)) {
                std::fprintf(stderr, "FAIL: %s\n", error.c_str());
                return 1;
            }
            // Guest calls require live game state: compile/load only here.
            std::printf("reported guest function %08X: compiled and loaded\n", pc);
        }
    }
    std::puts("runtime compiler: fresh DLL execution and warm-cache execution passed");
    return 0;
}
