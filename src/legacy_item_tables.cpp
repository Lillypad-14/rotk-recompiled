#include "legacy_item_tables.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "runtime_arm.h"

namespace rotk {
namespace {

constexpr std::uint32_t kItemTypeInfos = 0x08068F50u;
constexpr std::uint32_t kItemTypeCount = 15;
constexpr std::uint32_t kItemTypeInfoSize = 40;
constexpr std::uint32_t kRows = 256;
// Rows past the stock ones that patches add (Ringwraith gear reaches 0xAF) are copied too.
constexpr std::uint32_t kCopiedRows = 0xC0;
constexpr std::uint32_t kAlign = 16;
constexpr std::uint32_t kScratchSize = 4096;

// ItemTypeInfo pointer fields to copy and their row sizes. The prefix and suffix range tables
// stay in ROM: they bound loot rolls, which never pick a legacy row.
struct Field {
    std::uint32_t offset;
    std::uint32_t rowSize;
};
constexpr Field kFields[] = {
    {0, 12},   // baseItems: ItemStatRecord
    {8, 12},   // prefixes: ItemStatRecord
    {12, 4},   // prefixFlags: AffixFlags
    {20, 12},  // suffixes: ItemStatRecord
    {24, 4},   // suffixFlags: AffixFlags
    {28, 28},  // baseGfx: ItemGfx
    {32, 8},   // baseMetadata: ItemMetadata
};

std::unordered_map<std::uint32_t, std::uint32_t> g_redirect;  // ROM table address -> copy
std::uint32_t g_low = 0, g_high = 0;                          // bounds of g_redirect's keys
std::uint32_t g_infos = 0;                                    // the ItemTypeInfos copy
bool g_reading = false;                                       // reads while copying see the ROM

std::uint32_t alignUp(std::uint32_t value)
{
    return (value + kAlign - 1) & ~(kAlign - 1);
}

void copyWords(std::uint32_t to, std::uint32_t from, std::uint32_t bytes)
{
    for (std::uint32_t i = 0; i < bytes; i += 4) bus_write_u32(to + i, bus_read_u32(from + i));
}

void zeroWords(std::uint32_t to, std::uint32_t bytes)
{
    for (std::uint32_t i = 0; i < bytes; i += 4) bus_write_u32(to + i, 0u);
}

}  // namespace

std::uint32_t legacyItemTablesSize()
{
    // The mod's scratch, the ItemTypeInfos copy and at most one table per type and field.
    std::uint32_t size = kScratchSize + alignUp(kItemTypeCount * kItemTypeInfoSize);
    for (const Field& field : kFields) size += kItemTypeCount * alignUp(kRows * field.rowSize);
    return size;
}

void legacyItemTablesWrite(std::uint32_t address)
{
    legacyItemTablesReset();
    g_reading = true;
    zeroWords(address, kScratchSize);
    g_infos = address + kScratchSize;
    std::uint32_t cursor = alignUp(g_infos + kItemTypeCount * kItemTypeInfoSize);
    copyWords(g_infos, kItemTypeInfos, kItemTypeCount * kItemTypeInfoSize);
    std::unordered_map<std::uint32_t, std::uint32_t> copies;
    for (std::uint32_t type = 0; type < kItemTypeCount; ++type) {
        for (const Field& field : kFields) {
            const std::uint32_t slot = g_infos + type * kItemTypeInfoSize + field.offset;
            const std::uint32_t table = bus_read_u32(slot);
            if (table < 0x08000000u || table >= 0x0A000000u) continue;
            auto copy = copies.find(table);
            if (copy == copies.end()) {
                copy = copies.emplace(table, cursor).first;
                copyWords(cursor, table, kCopiedRows * field.rowSize);
                zeroWords(cursor + kCopiedRows * field.rowSize, (kRows - kCopiedRows) * field.rowSize);
                cursor = alignUp(cursor + kRows * field.rowSize);
            }
            bus_write_u32(slot, copy->second);
        }
    }
    g_reading = false;
    g_redirect = std::move(copies);
    g_redirect.emplace(kItemTypeInfos, g_infos);
    g_low = 0xFFFFFFFFu, g_high = 0;
    for (const auto& [table, copy] : g_redirect) g_low = std::min(g_low, table), g_high = std::max(g_high, table);
}

bool legacyItemTablesPresent(std::uint32_t address)
{
    return g_infos == address + kScratchSize && bus_read_u32(g_infos) != 0;
}

std::uint32_t legacyItemTablesInfos(std::uint32_t address)
{
    return address + kScratchSize;
}

void legacyItemTablesReset()
{
    g_redirect.clear();
    g_infos = 0;
    g_low = 1, g_high = 0;
}

bool legacyItemTablesRedirect(std::uint32_t original, std::uint32_t* out)
{
    if (g_reading || original < g_low || original > g_high + kItemTypeCount * kItemTypeInfoSize) return false;
    if (original - kItemTypeInfos < kItemTypeCount * kItemTypeInfoSize) {
        // ItemTypeInfos itself, or a field of one entry (ItemTypeInfos + 20 = ItemTypeInfos[0].suffixes)
        *out = g_infos + (original - kItemTypeInfos);
        return true;
    }
    const auto copy = g_redirect.find(original);
    if (copy == g_redirect.end()) return false;
    *out = copy->second;
    return true;
}

}  // namespace rotk
