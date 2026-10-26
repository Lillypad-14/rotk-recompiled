#include "hd_icons.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "gba_high_resolution.h"
#include "mod_runtime.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace rotk {
namespace {

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "hd-icons";
constexpr int kDefaultScale = 4;

std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return {};
    return std::filesystem::path(path).parent_path();
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

bool parseTiles(const std::string& hex, std::vector<std::uint8_t>& out)
{
    if (hex.size() % 2) return false;
    out.clear();
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        const std::string byte = hex.substr(i, 2);
        out.push_back(static_cast<std::uint8_t>(std::strtoul(byte.c_str(), &end, 16)));
        if (*end) return false;
    }
    return true;
}

// One pack: manifest.txt lines `<image.png> <tiles_w> <tiles_h> <4bpp tile data in hex>`, written
// by the romhack's hacks/tools/skill_icon_art.py build --hd-out.
void loadPack(const std::filesystem::path& directory, std::vector<gba::HdTileImage>& images)
{
    std::ifstream manifest(directory / "manifest.txt");
    std::string line;
    while (std::getline(manifest, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string file, hex;
        gba::HdTileImage image;
        if (!(fields >> file >> image.tiles_w >> image.tiles_h >> hex) || !parseTiles(hex, image.tiles) ||
            image.tiles.size() != std::size_t(image.tiles_w) * image.tiles_h * 32u) {
            std::fprintf(stderr, "[rotk] hd icons: bad line in %s\n", (directory / "manifest.txt").string().c_str());
            continue;
        }
        int width = 0, height = 0, channels = 0;
        const std::string path = (directory / file).string();
        stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!pixels) {
            std::fprintf(stderr, "[rotk] hd icons: cannot read %s\n", path.c_str());
            continue;
        }
        image.width = static_cast<unsigned>(width);
        image.height = static_cast<unsigned>(height);
        image.rgba.assign(pixels, pixels + std::size_t(width) * height * 4);
        stbi_image_free(pixels);
        for (std::size_t i = 0; i < image.rgba.size(); i += 4)
            for (std::size_t c = 0; c < 3; ++c)
                image.rgba[i + c] = static_cast<std::uint8_t>((image.rgba[i + c] * image.rgba[i + 3] + 127) / 255);
        images.push_back(std::move(image));
    }
}

void hdIconsActivated()
{
    const char* option = gba_mod_option_value(kPackageId, kFeatureId, "resolution");
    const int scale = option ? std::atoi(option) : kDefaultScale;
    std::vector<gba::HdTileImage> images;
    std::error_code error;
    const std::filesystem::path packages = executableDirectory() / "mods" / "packages" / kPackageId;
    for (const auto& version : std::filesystem::directory_iterator(packages, error)) {
        const std::filesystem::path hd = version.path() / "hd";
        for (const auto& pack : std::filesystem::directory_iterator(hd, error))
            if (pack.is_directory()) loadPack(pack.path(), images);
    }
    std::fprintf(stderr, "[rotk] hd icons: %zu images, internal resolution %dx\n", images.size(), scale);
    if (images.empty()) return;
    gba::hd_tiles_set(std::move(images));
    gba_mod_set_internal_resolution(scale);
}

void hdIconsReset()
{
    gba::hd_tiles_set({});
}

// The renderer needs a title provider before it accepts a higher internal resolution. ROTK's
// affine layers keep their native sampling.
bool noAffineLayers(int, int)
{
    return false;
}

}  // namespace

void hdIconsRegister(gbarecomp::RunOptions& options)
{
    gba_mod_register_activation_plugin("rotk.pc.hd-icons", hdIconsActivated);
    gba_mod_register_reset_callback(hdIconsReset);
    options.high_resolution_affine_provider = noAffineLayers;
}

}  // namespace rotk
