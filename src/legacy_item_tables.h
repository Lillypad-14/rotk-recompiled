#pragma once

#include <cstdint>

namespace rotk {

// Legacy item tables: host RAM copies of ItemTypeInfos and every item table it points to, 256 rows
// each, so the mod can add rows for items imported from Two Towers saves (legacy items). Rows
// below 0xA0 copy the ROM, rows 0xA0..0xFF start zeroed for the mod. ROM literals that point at
// ItemTypeInfos or one of its tables are redirected to the copies, so stock and mod code read the
// extended tables. Item counts stay as they are, so loot never rolls a legacy row.

// Host RAM the tables need, reserved by the layout.
std::uint32_t legacyItemTablesSize();
// Copies the tables from the ROM to a host RAM address and turns the redirect on.
void legacyItemTablesWrite(std::uint32_t address);
// Whether the tables are at a host RAM address of the active console, with the redirect on.
bool legacyItemTablesPresent(std::uint32_t address);
// Turns the redirect off (a layout without the tables).
void legacyItemTablesReset();
// The ROM read redirect: true with *out set when original points at a copied table.
bool legacyItemTablesRedirect(std::uint32_t original, std::uint32_t* out);

}  // namespace rotk
