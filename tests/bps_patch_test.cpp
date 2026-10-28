// Applies a BPS patch to a ROM and prints the result's SHA-1: bps_patch_test <rom> <patch> [out].
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bps_patch.h"
#include "sha1.h"

static std::vector<std::uint8_t> load(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    std::vector<std::uint8_t> target;
    std::string error;
    if (!rotk::bpsApply(load(argv[1]), load(argv[2]), target, &error)) {
        std::printf("failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("ok %zu bytes sha1 %s\n", target.size(), gba::sha1(target.data(), target.size()).hex().c_str());
    if (argc > 3) std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(target.data()), target.size());
    return 0;
}
