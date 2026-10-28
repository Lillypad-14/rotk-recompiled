// Builds The Two Towers data block from a ROM and checks its tables and strings:
// two_towers_test <Two Towers ROM>.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "expanded_edition.h"
#include "two_towers.h"

namespace rotk {
std::filesystem::path executableFolder() { return {}; }
std::string sessionRomPath(const std::vector<std::string>& args) { return args.size() > 1 ? args[1] : ""; }
}  // namespace rotk

static constexpr std::uint32_t kAt = 0x12345670u;
static std::vector<std::uint8_t> g_block;

static std::uint32_t word(std::uint32_t address)
{
    std::uint32_t value;
    std::memcpy(&value, g_block.data() + (address - kAt), 4);
    return value;
}

static std::string text(int language, std::uint32_t id)
{
    const std::uint32_t at = word(word(kAt + 0x20 + language * 4) + id * 4);
    return at ? reinterpret_cast<const char*>(g_block.data() + (at - kAt)) : "(none)";
}

static int g_failures = 0;

static void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += !ok;
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    rotk::twoTowersFind({argv[0], argv[1]});
    g_block = rotk::twoTowersBlockAt(kAt);
    expect(g_block.size() > 0x40 && word(kAt) == 0x31445454u, "magic, " + std::to_string(g_block.size() / 1024) + " KiB");
    if (g_failures) return 1;
    expect(word(kAt + 0x08) == 152 && word(kAt + 0x10) == 127 && word(kAt + 0x18) == 58, "table counts");
    std::uint16_t staffName;
    std::memcpy(&staffName, g_block.data() + (word(kAt + 0x0C) + 0x24 * 16 - kAt), 2);
    expect(text(0, staffName) == "Short Staff", "base 0x24 in English: " + text(0, staffName));
    expect(text(1, staffName) == "Baguette", "base 0x24 in French: " + text(1, staffName));
    expect(text(0, 21) == "Gandalf", "hero 3 name: " + text(0, 21));
    expect(text(0, 271 + 0x31) == "Wisdom", "stat 0x31: " + text(0, 271 + 0x31));
    expect(text(0, 600) == "Leaf Sword" && text(0, 5) == "(none)", "item names carried, menu text not");
    return g_failures ? 1 : 0;
}
