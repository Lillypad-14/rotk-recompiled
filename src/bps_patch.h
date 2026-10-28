#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rotk {

// Applies a BPS patch to `source`, checking the source, target and patch CRC32s. On failure returns
// false with a reason a player can read.
bool bpsApply(const std::vector<std::uint8_t>& source, const std::vector<std::uint8_t>& patch,
              std::vector<std::uint8_t>& target, std::string* error);

}  // namespace rotk
