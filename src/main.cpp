#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

#include "gba_bus.h"
#include "gba_memory.h"
#include "gba_ppu.h"
#include "hd_audio.h"
#include "hd_icons.h"
#include "local_coop.h"
#include "smooth_motion.h"
#include "transparency.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "widescreen.h"

#if defined(ROTK_LAUNCHER)
#include <string>
#include <vector>

#include "launcher_boot.h"
#endif

extern "C" void (*g_dma_observer)(int channel, std::uint32_t src, std::uint32_t dst, std::uint32_t bytes);

namespace {

// PC engine services for ROTK. Each one moves a fixed GBA allocation into host RAM:
//
//   kHostRamBase  info block (PcInfo below), read by PC-aware mod code
//   +0x1000       per-actor queues (collision, world render, post-commit)
//   ...           main and overlay actor pools
//   ...           game heap
//
// The player picks them on the launcher's Mods page (built-in package "rotk.pc", see
// mods/rotk.pc.manifest.toml.in). Environment variables override the package for tests and scripts:
// ROTK_PC_HEAP_MB=0 keeps the EWRAM heap, ROTK_PC_ACTORS=256 and ROTK_PC_OVERLAY_ACTORS=40 keep the
// stock pools and queues.

constexpr std::uint32_t kMemoryAddHeap = 0x08032E74u;
constexpr std::uint32_t kPoolBuildFreeList = 0x080304E0u;
constexpr std::uint32_t kHeapArena = 0x02002640u;          // g_HeapArena
constexpr std::uint32_t kHeaps = 0x03002048u;              // Heap g_Heaps[1]
constexpr std::uint32_t kHeapCount = 0x03002058u;          // u32 g_HeapCount
constexpr std::uint32_t kActorPoolMain = 0x030015B8u;      // ActorPoolMain *g_ActorPoolMain
constexpr std::uint32_t kActorPoolOverlay = 0x030015BCu;   // ActorPoolOverlay *g_ActorPoolOverlay
constexpr std::uint32_t kCollisionCheckQueue = 0x03002430u;
constexpr std::uint32_t kWorldRenderQueue = 0x03002830u;
constexpr std::uint32_t kPostCommitQueue = 0x030015E8u;
constexpr std::uint32_t kWorldRenderQueueCount = 0x03003220u;
constexpr std::uint32_t kRenderSort = 0x03002C30u;       // g_ActorRenderSortCode (ARM, IWRAM)
constexpr std::uint32_t kHeapAlign = 16u;                  // sizeof(HeapBlock)
constexpr std::uint32_t kActorSize = 0x148u;               // sizeof(Actor)
constexpr std::uint32_t kStockMainActors = 0x100u;
constexpr std::uint32_t kStockOverlayActors = 0x28u;
constexpr std::uint32_t kInfoBytes = 0x1000u;
constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kHeapHookId = "rotk.pc-heap";
constexpr const char* kPoolHookId = "rotk.pc-actor-pools";
constexpr const char* kSortHookId = "rotk.pc-render-sort";
constexpr const char* kPushOamHookId = "rotk.pc-push-oam";
constexpr const char* kHideOamHookId = "rotk.pc-hide-oam";
constexpr const char* kEndFrameHookId = "rotk.pc-end-frame";

// Host RAM info block. Keep in step with the mod's hacks/lib/pc_host.h.
constexpr std::uint32_t kPcInfoMagic = 0x31435052u;  // "RPC1"
enum PcInfoField : std::uint32_t {
    kInfoMagic = 0x00,
    kInfoVersion = 0x04,
    kInfoMainActors = 0x08,
    kInfoOverlayActors = 0x0C,
    kInfoMainPool = 0x10,
    kInfoOverlayPool = 0x14,
    kInfoHeapBase = 0x18,
    kInfoHeapEnd = 0x1C,
    kInfoStressSpawn = 0x20,
};

// ROM literal-pool words that hold the base of a per-actor queue. The same addresses in the stock
// and Expanded ROMs. Each is replaced only while the ROM still holds the expected value.
struct LiteralRedirect {
    std::uint32_t romAddress;
    std::uint32_t stockValue;
};
constexpr LiteralRedirect kQueueLiterals[] = {
    {0x08002120u, kCollisionCheckQueue}, {0x0800D0D0u, kCollisionCheckQueue},
    {0x0800D1BCu, kCollisionCheckQueue}, {0x08000968u, kWorldRenderQueue},
    {0x080009DCu, kWorldRenderQueue},    {0x08002278u, kWorldRenderQueue},
    {0x0800E2A8u, kWorldRenderQueue},    {0x08000874u, kPostCommitQueue},
    {0x0800096Cu, kPostCommitQueue},     {0x080017B0u, kPostCommitQueue},
};
constexpr std::uint32_t kLastQueueLiteral = 0x0800E2A8u;

struct PcLayout {
    bool heap = false;
    bool actors = false;
    std::uint32_t mainActors = kStockMainActors;
    std::uint32_t overlayActors = kStockOverlayActors;
    std::uint32_t collisionQueue = 0;
    std::uint32_t worldRenderQueue = 0;
    std::uint32_t postCommitQueue = 0;
    std::uint32_t mainPool = 0;
    std::uint32_t overlayPool = 0;
    std::uint32_t heapBase = 0;
    std::uint32_t heapEnd = 0;
};

unsigned long envNumber(const char* name, unsigned long fallback)
{
    const char* value = std::getenv(name);
    return value ? std::strtoul(value, nullptr, 10) : fallback;
}

// A PC setting: the environment variable when set, else the option of the enabled package feature,
// else the stock value. Package options are only available after the launcher commits them.
unsigned long pcSetting(const char* env, const char* feature, const char* option, unsigned long stock)
{
    if (const char* value = std::getenv(env)) return std::strtoul(value, nullptr, 10);
    if (const char* value = gba_mod_option_value(kPackageId, feature, option))
        return std::strtoul(value, nullptr, 10);
    return stock;
}

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

PcLayout makeLayout(bool coop = false)
{
    PcLayout layout;
    const unsigned long hostBytes =
        envNumber("GBARECOMP_HOST_RAM_MB", gba::kHostRamDefaultSize / (1024u * 1024u)) * 1024u * 1024u;
    unsigned long heapBytes = pcSetting("ROTK_PC_HEAP_MB", "large-memory", "heap-mb", 0) * 1024u * 1024u;
    unsigned long mainActors = pcSetting("ROTK_PC_ACTORS", "actor-limits", "main-actors", kStockMainActors);
    unsigned long overlayActors =
        pcSetting("ROTK_PC_OVERLAY_ACTORS", "actor-limits", "overlay-actors", kStockOverlayActors);
    if (coop) {
        // Expanded link play needs the same pool/queue/heap contract on both
        // consoles, including when the solo engine enhancements are disabled.
        if (heapBytes < 4u * 1024u * 1024u) heapBytes = 4u * 1024u * 1024u;
        if (mainActors < 1024u) mainActors = 1024u;
        if (overlayActors < 160u) overlayActors = 160u;
    }
    if (mainActors < kStockMainActors) mainActors = kStockMainActors;
    if (overlayActors < kStockOverlayActors) overlayActors = kStockOverlayActors;
    layout.actors = mainActors != kStockMainActors || overlayActors != kStockOverlayActors;

    std::uint32_t cursor = gba::kHostRamBase + kInfoBytes;
    if (layout.actors) {
        const std::uint32_t queueBytes = static_cast<std::uint32_t>(mainActors + overlayActors) * 4u;
        layout.mainActors = static_cast<std::uint32_t>(mainActors);
        layout.overlayActors = static_cast<std::uint32_t>(overlayActors);
        layout.collisionQueue = cursor;
        layout.worldRenderQueue = cursor + queueBytes;
        layout.postCommitQueue = cursor + 2u * queueBytes;
        cursor = alignUp(cursor + 3u * queueBytes, kHeapAlign);
        layout.mainPool = cursor;
        layout.overlayPool = alignUp(cursor + layout.mainActors * kActorSize, kHeapAlign);
        cursor = alignUp(layout.overlayPool + layout.overlayActors * kActorSize, kHeapAlign);
    }
    if (heapBytes != 0) {
        layout.heap = true;
        layout.heapBase = cursor;
        layout.heapEnd = cursor + static_cast<std::uint32_t>(heapBytes);
        cursor = layout.heapEnd;
    }
    if (cursor - gba::kHostRamBase > hostBytes) {
        std::fprintf(stderr, "[rotk] PC layout needs %u KiB of host RAM, have %lu; stock heap and actors\n",
                     (cursor - gba::kHostRamBase) / 1024u, hostBytes / 1024u);
        return PcLayout{};
    }
    if (layout.heap)
        std::fprintf(stderr, "[rotk] game heap: %lu MiB of host RAM\n", heapBytes / (1024u * 1024u));
    else
        std::fprintf(stderr, "[rotk] stock EWRAM heap\n");
    std::fprintf(stderr, "[rotk] actor pools: %u main, %u overlay\n", layout.mainActors,
                 layout.overlayActors);
    return layout;
}

PcLayout g_layout;
PcLayout g_soloLayout;
bool g_coopEngineLayout = false;
constexpr const char* kCoopEngineHooks[] = {kHeapHookId, kPoolHookId, kSortHookId};
bool g_soloEngineHooks[3] = {};
int (*g_soloQueueOverride)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;

const PcLayout& layout()
{
    return g_layout;
}

void writeInfoBlock()
{
    const PcLayout& pc = layout();
    const std::uint32_t info = gba::kHostRamBase;
    static bool stressArmed = false;
    bus_write_u32(info + kInfoVersion, 2u);
    bus_write_u32(info + kInfoMainActors, pc.mainActors);
    bus_write_u32(info + kInfoOverlayActors, pc.overlayActors);
    bus_write_u32(info + kInfoMainPool, pc.mainPool);
    bus_write_u32(info + kInfoOverlayPool, pc.overlayPool);
    bus_write_u32(info + kInfoHeapBase, pc.heapBase);
    bus_write_u32(info + kInfoHeapEnd, pc.heapEnd);
    if (!stressArmed) {
        // ROTK_PC_STRESS_SPAWN=N: the mod's debug lab spawns N enemies around the player once.
        stressArmed = true;
        bus_write_u32(info + kInfoStressSpawn,
                      static_cast<std::uint32_t>(envNumber("ROTK_PC_STRESS_SPAWN", 0)));
    }
    bus_write_u32(info + kInfoMagic, kPcInfoMagic);
}

void returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    else cpu->cpsr &= ~0x20u;
}

// memory_addHeap(0, g_HeapArena, end) becomes memory_addHeap(0, heapBase, heapEnd), done on the
// guest's behalf. Other memory_addHeap calls run as written.
int pcHeapInit(std::uint32_t, int, ArmCpuState* cpu)
{
    const PcLayout& pc = layout();
    if (!pc.heap || cpu->R[0] != 0u || cpu->R[1] != kHeapArena) return 0;
    const std::uint32_t firstBlock = pc.heapBase + kHeapAlign;

    writeInfoBlock();
    bus_write_u32(kHeaps + 0u, firstBlock);
    bus_write_u32(kHeaps + 4u, firstBlock);
    bus_write_u32(kHeaps + 8u, pc.heapEnd - pc.heapBase - kHeapAlign);
    bus_write_u32(kHeaps + 12u, pc.heapEnd);
    for (std::uint32_t field = 0; field < kHeapAlign; field += 4u)
        bus_write_u32(firstBlock + field, 0u);
    bus_write_u32(kHeapCount, bus_read_u32(kHeapCount) + 1u);
    returnToCaller(cpu);
    return 1;
}

// pool_buildFreeList(pool, count, size) at 0x080304E0: zero count slots of size bytes and push each
// onto a fresh free list (list_pushFront, so the last slot is the head). For the two actor pools the
// host builds the larger pool instead and repoints g_ActorPoolMain/g_ActorPoolOverlay at it. This runs
// at boot and on every area reset.
int pcActorPools(std::uint32_t, int, ArmCpuState* cpu)
{
    const PcLayout& pc = layout();
    std::uint32_t pool;
    std::uint32_t count;
    std::uint32_t poolPointer;

    if (!pc.actors || cpu->R[2] != kActorSize) return 0;
    if (cpu->R[1] == kStockMainActors) {
        pool = pc.mainPool;
        count = pc.mainActors;
        poolPointer = kActorPoolMain;
    } else if (cpu->R[1] == kStockOverlayActors) {
        pool = pc.overlayPool;
        count = pc.overlayActors;
        poolPointer = kActorPoolOverlay;
    } else {
        return 0;
    }
    writeInfoBlock();
    for (std::uint32_t offset = 0; offset < count * kActorSize; offset += 4u)
        bus_write_u32(pool + offset, 0u);
    std::uint32_t head = 0;
    for (std::uint32_t slot = 0; slot < count; ++slot) {
        const std::uint32_t actor = pool + slot * kActorSize;
        bus_write_u32(actor + 0u, head);  // next
        bus_write_u32(actor + 4u, 0u);    // previous
        if (head != 0) bus_write_u32(head + 4u, actor);
        head = actor;
    }
    bus_write_u32(poolPointer, pool);
    cpu->R[0] = head;
    returnToCaller(cpu);
    return 1;
}

// Render order key of the IWRAM sort: OAM priority, then the pinned-front/back flags, then lower
// screen y first. Compared unsigned.
std::uint32_t renderSortKey(std::uint32_t actor)
{
    const std::uint32_t flags = bus_read_u32(actor + 0xE8u);
    std::uint32_t key = (static_cast<std::uint32_t>(bus_read_u8(actor + 0xE1u)) << 22) & 0x03000000u;
    if (flags & 0x00080000u) key += 0x20000u;
    else if (!(flags & 0x00100000u)) key += 0x10000u;
    return key + 0x10000u - static_cast<std::uint32_t>(static_cast<std::int16_t>(bus_read_u16(actor + 0xD2u)));
}

// g_ActorRenderSortCode (0x03002C30, ARM, copied from 0x0800E208): Shell sort of
// g_SpriteWorldRenderQueue with gaps 21, 7, 3, 1. The original loads the count with ldrb, so 256 or
// more queued actors sort only count & 0xFF of them. Same algorithm here with the full count, so the
// order is identical below 256.
int pcRenderSort(std::uint32_t, int, ArmCpuState* cpu)
{
    const PcLayout& pc = layout();
    if (!pc.actors) return 0;
    const std::uint32_t queue = pc.worldRenderQueue;
    const std::uint32_t count = bus_read_u32(kWorldRenderQueueCount);
    static std::uint32_t peak = 0;
    if (peak == 0) std::fprintf(stderr, "[rotk] host render sort active (%u queued)\n", count);
    if (count >= peak + 32u) {
        peak = count;
        std::fprintf(stderr, "[rotk] render queue peak %u\n", count);
    } else if (peak == 0) {
        peak = 1;
    }
    for (std::uint32_t gap : {21u, 7u, 3u, 1u}) {
        for (std::uint32_t i = gap; i < count; ++i) {
            const std::uint32_t actor = bus_read_u32(queue + i * 4u);
            const std::uint32_t key = renderSortKey(actor);
            std::uint32_t j = i;
            while (j >= gap) {
                const std::uint32_t other = bus_read_u32(queue + (j - gap) * 4u);
                if (key >= renderSortKey(other)) break;
                bus_write_u32(queue + j * 4u, other);
                j -= gap;
            }
            bus_write_u32(queue + j * 4u, actor);
        }
    }
    returnToCaller(cpu);
    return 1;
}

// Repoint the per-actor queue literals at host RAM queues sized for every actor.
int pcQueueLiterals(std::uint32_t address, std::uint32_t original, std::uint32_t* out)
{
    const std::uint32_t romAddress = 0x08000000u | (address & 0x01FFFFFFu);
    if (romAddress > kLastQueueLiteral) return 0;
    const PcLayout& pc = layout();
    for (const LiteralRedirect& literal : kQueueLiterals) {
        if (literal.romAddress != romAddress || literal.stockValue != original) continue;
        *out = literal.stockValue == kCollisionCheckQueue ? pc.collisionQueue
               : literal.stockValue == kWorldRenderQueue  ? pc.worldRenderQueue
                                                           : pc.postCommitQueue;
        return 1;
    }
    return 0;
}


// Extra sprite pieces beyond the 128 OAM entries. sprite_pushOam(index, entry) stops at 128
// (g_SpriteAllocCount stays 128 from then on); the host keeps the rest in extra pages that mirror
// the guest's shadow OAM: one per shadow buffer (g_OamShadowA/B) and parity half, written alike by
// pushes and differing only by sprite_hideOamEntry, which edits the second half. The VBlank
// handler DMAs one half of one buffer to OAM, and the PPU draws the matching extra page after the
// hardware entries.
constexpr std::uint32_t kPushOam = 0x08036540u;
constexpr std::uint32_t kHideOamEntry = 0x08036590u;
constexpr std::uint32_t kEndFrame = 0x080366A4u;
constexpr std::uint32_t kSpriteAllocCount = 0x03004220u;  // u8
constexpr std::uint32_t kOamShadow = 0x03004B40u;         // u8 *g_OamShadow
constexpr std::uint32_t kOamShadowA = 0x03004340u;
constexpr std::uint32_t kOamShadowB = 0x03004B80u;
constexpr std::uint32_t kOamHalfBytes = 0x400u;
constexpr std::uint32_t kOamAddress = 0x07000000u;
constexpr std::uint32_t kHardwareObjects = 128u;
struct ExtraObjects {
    std::uint32_t capacity = 0;
    std::uint32_t hardwareLimit = kHardwareObjects;  // OAM entries used before extra pages
    std::uint8_t* pages[2][2] = {};    // [shadow buffer][half], capacity entries of 8 bytes
    std::uint32_t counts[2] = {};      // pieces per shadow buffer
    int displayedBuffer = -1;          // buffer whose half is in hardware OAM
    int displayedHalf = 0;
    bool lastPushExtra = false;        // the latest push went to an extra page
};

ExtraObjects g_extra;                 // the solo game's
ExtraObjects g_coopExtra[2];          // the split-screen co-op consoles'
ExtraObjects* g_currentExtra = &g_extra;

ExtraObjects& extraObjects()
{
    return *g_currentExtra;
}

void freePages(ExtraObjects& extra)
{
    for (auto& buffer : extra.pages)
        for (auto& half : buffer) {
            delete[] half;
            half = nullptr;
        }
}

// Sizes the extra pages from the "sprite-limits" feature (total pieces) or ROTK_PC_EXTRA_OBJECTS
// (extra pieces). Runs before boot, so no frame is in flight.
void configureExtraObjects()
{
    freePages(g_extra);
    g_extra = ExtraObjects{};
    const unsigned long total = pcSetting("ROTK_PC_MAX_SPRITES", "sprite-limits", "max-sprites", kHardwareObjects);
    unsigned long capacity = total > kHardwareObjects ? total - kHardwareObjects : 0;
    if (const char* value = std::getenv("ROTK_PC_EXTRA_OBJECTS")) capacity = std::strtoul(value, nullptr, 10);
    if (capacity > 3800u) capacity = 3800u;
    g_extra.capacity = static_cast<std::uint32_t>(capacity);
    // ROTK_PC_OAM_LIMIT (test only): send pieces past this many OAM entries to the extra pages.
    // The picture must match an unlimited run, since extra pieces keep their order behind OAM.
    g_extra.hardwareLimit = static_cast<std::uint32_t>(envNumber("ROTK_PC_OAM_LIMIT", kHardwareObjects));
    if (g_extra.hardwareLimit < 1u || g_extra.hardwareLimit > kHardwareObjects) g_extra.hardwareLimit = kHardwareObjects;
    if (g_extra.capacity == 0) return;
    for (auto& buffer : g_extra.pages)
        for (auto& half : buffer) half = new std::uint8_t[g_extra.capacity * 8u]();
    std::fprintf(stderr, "[rotk] sprite pieces: %u (128 OAM + %u extra)\n",
                 kHardwareObjects + g_extra.capacity, g_extra.capacity);
}

int shadowBuffer(std::uint32_t shadow)
{
    return shadow == kOamShadowA ? 0 : shadow == kOamShadowB ? 1 : -1;
}

int pcPushOam(std::uint32_t, int, ArmCpuState* cpu)
{
    ExtraObjects& extra = extraObjects();
    const int buffer = shadowBuffer(bus_read_u32(kOamShadow));
    const std::uint32_t count = bus_read_u8(kSpriteAllocCount);
    if (buffer < 0) return 0;
    if (count == 0) extra.counts[buffer] = 0;
    if (count < extra.hardwareLimit) {
        extra.lastPushExtra = false;
        rotk::widescreenNotePiece(buffer, count);
        rotk::smoothNotePiece(buffer, count);
        return 0;
    }
    if (extra.counts[buffer] >= extra.capacity) {
        extra.lastPushExtra = false;
        cpu->R[0] = 0xFFFFFFFFu;
        returnToCaller(cpu);
        return 1;
    }
    static std::uint32_t peak = 0;
    if (extra.counts[buffer] + 1u >= peak + 16u) {
        peak = extra.counts[buffer] + 1u;
        std::fprintf(stderr, "[rotk] extra sprite pieces peak %u\n", peak);
    }
    rotk::widescreenNotePiece(buffer, kHardwareObjects + extra.counts[buffer]);
    rotk::smoothNotePiece(buffer, kHardwareObjects + extra.counts[buffer]);
    const std::uint32_t slot = extra.counts[buffer]++ * 8u;
    for (std::uint32_t i = 0; i < 8u; ++i) {
        const std::uint8_t value = bus_read_u8(cpu->R[1] + i);
        extra.pages[buffer][0][slot + i] = value;
        extra.pages[buffer][1][slot + i] = value;
    }
    extra.lastPushExtra = true;
    cpu->R[0] = 0;
    returnToCaller(cpu);
    return 1;
}

// sprite_hideOamEntry(g_SpriteAllocCount - 1) right after an extra push names the last OAM entry, but means
// the extra piece: disable it in the second half, as the original does for OAM entries.
int pcHideOamEntry(std::uint32_t, int, ArmCpuState* cpu)
{
    ExtraObjects& extra = extraObjects();
    const int buffer = shadowBuffer(bus_read_u32(kOamShadow));
    if (!extra.lastPushExtra || buffer < 0 || cpu->R[0] != extra.hardwareLimit - 1u ||
        extra.counts[buffer] == 0)
        return 0;
    std::uint8_t& attr0High = extra.pages[buffer][1][(extra.counts[buffer] - 1u) * 8u + 1u];
    attr0High = static_cast<std::uint8_t>((attr0High & ~3u) | 2u);
    returnToCaller(cpu);
    return 1;
}

// Observes sprite_endFrame: a frame that pushed nothing leaves no extra pieces behind.
int pcEndFrame(std::uint32_t, int, ArmCpuState*)
{
    ExtraObjects& extra = extraObjects();
    const int buffer = shadowBuffer(bus_read_u32(kOamShadow));
    if (buffer >= 0 && bus_read_u8(kSpriteAllocCount) == 0) extra.counts[buffer] = 0;
    extra.lastPushExtra = false;
    rotk::smoothEndFrame(buffer);
    return 0;
}

void pcObserveDma(int, std::uint32_t source, std::uint32_t destination, std::uint32_t bytes)
{
    ExtraObjects& extra = extraObjects();
    rotk::widescreenObserveDma(source, destination);
    if (destination != kOamAddress) return;
    extra.displayedBuffer = -1;
    if (bytes != kOamHalfBytes) return;
    for (int half = 0; half < 2; ++half) {
        const int buffer = shadowBuffer(source - static_cast<std::uint32_t>(half) * kOamHalfBytes);
        if (buffer >= 0) {
            extra.displayedBuffer = buffer;
            extra.displayedHalf = half;
        }
    }
    rotk::widescreenSetDisplayedBuffer(extra.displayedBuffer);
    rotk::smoothSetDisplayedBuffer(extra.displayedBuffer);
    rotk::transparencySetDisplayed(extra.displayedBuffer, extra.displayedHalf);
}

const std::uint8_t* pcExtraObjects(std::uint32_t* count)
{
    const ExtraObjects& extra = extraObjects();
    if (extra.displayedBuffer < 0) {
        *count = 0;
        return nullptr;
    }
    *count = extra.counts[extra.displayedBuffer];
    const std::uint8_t* const* pages = extra.pages[extra.displayedBuffer];
    const std::uint8_t* pieces =
        rotk::transparencyAdjustExtra(pages[extra.displayedHalf], pages[0], pages[1], *count);
    return rotk::smoothAdjustExtra(pieces, *count);
}

void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Stereo sound: the audio device opens after mod activation and reads GBARECOMP_AUDIO_STEREO.
// Reset callbacks run before activation, so this picks mono and the feature's activation switches
// back to stereo. A variable set by the player wins.
bool g_stereoFromEnv = false;

void applyStereoDefault()
{
    if (!g_stereoFromEnv) setEnv("GBARECOMP_AUDIO_STEREO", "0");
}

void stereoActivated()
{
    if (!g_stereoFromEnv) setEnv("GBARECOMP_AUDIO_STEREO", "1");
}

// CPU boost: GBARECOMP_CPU_OVERCLOCK is read on the first emulated instruction, after this runs.
void applyCpuBoost()
{
    if (std::getenv("GBARECOMP_CPU_OVERCLOCK")) return;
    const char* speed = gba_mod_option_value(kPackageId, "cpu-boost", "speed");
    if (!speed) return;
#if defined(_WIN32)
    _putenv_s("GBARECOMP_CPU_OVERCLOCK", speed);
#else
    setenv("GBARECOMP_CPU_OVERCLOCK", speed, 1);
#endif
}

// Resolves every PC setting and enables the matching services. Runs as a mod reset callback, after
// the launcher's package choices are committed and before the game boots. Mod activation disables
// every entry hook first, so a feature that is off leaves the stock code running.
void applyPcSettings()
{
    g_layout = makeLayout();
    configureExtraObjects();
    applyCpuBoost();
    applyStereoDefault();
    rotk::smoothMotionApply();
    rotk::transparencyApply();
    const bool extra = extraObjects().capacity != 0;
    const bool observePieces = extra || rotk::smoothMotionActive();
    gba_mod_set_function_hook_enabled(kHeapHookId, layout().heap);
    gba_mod_set_function_hook_enabled(kPoolHookId, layout().actors);
    gba_mod_set_function_hook_enabled(kSortHookId,
                                      layout().actors && envNumber("ROTK_PC_RENDER_SORT", 1) != 0);
    gba_mod_set_function_hook_enabled(kPushOamHookId, observePieces);
    gba_mod_set_function_hook_enabled(kHideOamHookId, observePieces);
    gba_mod_set_function_hook_enabled(kEndFrameHookId, observePieces);
    gba::g_rom_read32_override = layout().actors ? pcQueueLiterals : nullptr;
    g_dma_observer = pcObserveDma;
    gba::g_extra_obj_provider = extra ? pcExtraObjects : nullptr;
    rotk::hdAudioApply();
}

// Every package feature names a trusted plugin. The settings themselves are read in
// applyPcSettings, so the plugins only need to exist.
void featureActivated() {}

}  // namespace

namespace rotk {

// Split-screen co-op (src/local_coop.cpp): the presentation features keep one state per console.
// Each co-op console uses an independent engine layout in its own host RAM.
// The frozen solo machine's layout is restored before it resumes.
void pcFeaturesStartCoop()
{
    if (!g_coopEngineLayout) {
        const PcLayout coop = makeLayout(true);
        if (!coop.heap || !coop.actors)
            throw std::runtime_error("Split-screen co-op needs more host RAM for its engine buffers");
        g_soloLayout = g_layout;
        g_layout = coop;
        g_soloQueueOverride = gba::g_rom_read32_override;
        for (int i = 0; i < 3; ++i) {
            g_soloEngineHooks[i] = gba_mod_function_hook_enabled(kCoopEngineHooks[i]) != 0;
            gba_mod_set_function_hook_enabled(kCoopEngineHooks[i], 1);
        }
        g_coopEngineLayout = true;
    }
    gba::g_rom_read32_override = pcQueueLiterals;
    for (ExtraObjects& extra : g_coopExtra) {
        freePages(extra);
        extra = ExtraObjects{};
        extra.capacity = g_extra.capacity;
        extra.hardwareLimit = g_extra.hardwareLimit;
        if (extra.capacity)
            for (auto& buffer : extra.pages)
                for (auto& half : buffer) half = new std::uint8_t[extra.capacity * 8u]();
    }
    for (const char* id : {kPushOamHookId, kHideOamHookId, kEndFrameHookId, kHeapHookId, kPoolHookId, kSortHookId})
        gba_mod_set_function_hook_runs_suspended(id, 1);
    widescreenStartCoop();
    smoothStartCoop();
    transparencyStartCoop();
    rotk::hdAudioStartCoop();
}

void pcFeaturesEndCoop()
{
    for (const char* id : {kPushOamHookId, kHideOamHookId, kEndFrameHookId, kHeapHookId, kPoolHookId, kSortHookId})
        gba_mod_set_function_hook_runs_suspended(id, 0);
    widescreenEndCoop();
    smoothEndCoop();
    rotk::hdAudioEndCoop();
    pcFeaturesSelectConsole(-1);
    if (g_coopEngineLayout) {
        g_layout = g_soloLayout;
        gba::g_rom_read32_override = g_soloQueueOverride;
        for (int i = 0; i < 3; ++i)
            gba_mod_set_function_hook_enabled(kCoopEngineHooks[i], g_soloEngineHooks[i]);
        g_coopEngineLayout = false;
    }
}

void pcFeaturesSelectConsole(int console)
{
    g_currentExtra = console >= 0 && console < 2 ? &g_coopExtra[console] : &g_extra;
    widescreenSelectConsole(console);
    smoothSelectConsole(console);
    transparencySelectConsole(console);
}

}  // namespace rotk

int main(int argc, char** argv)
{
    gba_mod_register_function_entry_plugin(kHeapHookId, kMemoryAddHeap, 1, pcHeapInit);
    gba_mod_register_function_entry_plugin(kPoolHookId, kPoolBuildFreeList, 1, pcActorPools);
    gba_mod_register_function_entry_plugin(kSortHookId, kRenderSort, 0, pcRenderSort);
    gba_mod_register_function_entry_plugin(kPushOamHookId, kPushOam, 1, pcPushOam);
    gba_mod_register_function_entry_plugin(kHideOamHookId, kHideOamEntry, 1, pcHideOamEntry);
    gba_mod_register_function_entry_plugin(kEndFrameHookId, kEndFrame, 1, pcEndFrame);
    for (const char* plugin : {"rotk.pc.large-memory", "rotk.pc.actor-limits", "rotk.pc.sprite-limits",
                               "rotk.pc.cpu-boost"})
        gba_mod_register_activation_plugin(plugin, featureActivated);
    rotk::hdAudioRegister();
    rotk::smoothMotionRegister();
    rotk::transparencyRegister();
    g_stereoFromEnv = std::getenv("GBARECOMP_AUDIO_STEREO") != nullptr;
    gba_mod_register_activation_plugin("rotk.pc.stereo", stereoActivated);
    gba_mod_register_reset_callback(applyPcSettings);

    gbarecomp::RunOptions options;
    options.builtin_game_name = ROTK_GAME_NAME;
    options.builtin_rom_sha1 = ROTK_ROM_SHA1;
    options.builtin_rom_crc32 = ROTK_ROM_CRC32;
    options.mod_game_id = ROTK_MOD_GAME_ID;
    rotk::widescreenRegister(options);
    rotk::hdIconsRegister(options);
    rotk::localCoopRegister(options);
#if defined(ROTK_LAUNCHER)
    std::vector<std::string> args(argv, argv + argc);
    if (rotk_launcher_preboot(args, options)) return 0;  // the player quit the launcher
    std::vector<char*> launchArgs;
    for (std::string& arg : args) launchArgs.push_back(arg.data());
    return gbarecomp::run_game(static_cast<int>(launchArgs.size()), launchArgs.data(), options);
#else
    return gbarecomp::run_game(argc, argv, options);
#endif
}
