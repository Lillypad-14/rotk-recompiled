#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rotk {

// The Two Towers data block: what the mod needs from The Two Towers ROM to show and use items
// imported from Two Towers saves (legacy items), copied into host RAM after the heap. The mod
// finds it through the info block (PcHostInfo.twoTowers, version 4). Layout, all little-endian
// u32 with host RAM addresses:
//
//   0x00  magic "TTD1"
//   0x04  prefix records, 0x08 their count
//   0x0C  base item records, 0x10 their count
//   0x14  suffix records, 0x18 their count
//   0x1C  string count
//   0x20  per ROTK language (EN, FR, DE, IT, ES): a u32 table of string addresses, 0 for a
//         string not carried
//
// Item records are Two Towers' own 16 bytes: u16 name string, s16 gem value, 5 x (u8 stat,
// s8 value) with stat 0x40 for none. Strings are NUL-terminated font bytes.

// Finds The Two Towers ROM (ROTK_PC_TWO_TOWERS_ROM, else beside the ROTK ROM or the executable),
// checks its SHA-1 and builds the block. Without the ROM the block is empty.
void twoTowersFind(const std::vector<std::string>& args);
// The block's size in bytes, 0 without The Two Towers.
std::uint32_t twoTowersBlockSize();
// The block as it reads at a host RAM address.
std::vector<std::uint8_t> twoTowersBlockAt(std::uint32_t address);
// Writes the block at a host RAM address.
void twoTowersWrite(std::uint32_t address);

}  // namespace rotk
