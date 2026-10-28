#include "color_vision.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mod_runtime.h"

namespace rotk {
namespace {

// Color vision correction (daltonization): each shown frame's colors are recolored so that the
// differences a color vision deficiency hides move into colors it keeps. Presentation only.
// The correction goes through a table of 64 levels per channel, built when the feature is set.

constexpr const char* kPackageId = "rotk.pc";
constexpr const char* kFeatureId = "color-vision";
constexpr int kLevels = 64;

enum class Mode { Off, Protan, Deutan, Tritan };

Mode g_mode = Mode::Off;
std::vector<std::uint8_t> g_table;  // kLevels^3 RGB triples

using Matrix = float[3][3];

// Linear RGB to LMS and back (Vienot, Brettel and Mollon 1999).
constexpr Matrix kRgbToLms = {{17.8824f, 43.5161f, 4.11935f},
                              {3.45565f, 27.1554f, 3.86714f},
                              {0.0299566f, 0.184309f, 1.46709f}};
constexpr Matrix kLmsToRgb = {{0.0809444479f, -0.130504409f, 0.116721066f},
                              {-0.0102485335f, 0.0540193266f, -0.113614708f},
                              {-0.000365296938f, -0.00412161469f, 0.693511405f}};
// LMS as seen with each deficiency.
constexpr Matrix kSimulate[3] = {
    {{0.0f, 2.02344f, -2.52581f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{1.0f, 0.0f, 0.0f}, {0.494207f, 0.0f, 1.24827f}, {0.0f, 0.0f, 1.0f}},
    {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {-0.395913f, 0.801109f, 0.0f}},
};

void apply(const Matrix& m, const float in[3], float out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
}

void buildTable(Mode mode)
{
    const Matrix& simulate = kSimulate[static_cast<int>(mode) - 1];
    g_table.resize(std::size_t(kLevels) * kLevels * kLevels * 3u);
    std::size_t at = 0;
    for (int r = 0; r < kLevels; ++r)
        for (int g = 0; g < kLevels; ++g)
            for (int b = 0; b < kLevels; ++b) {
                const float rgb[3] = {r * 255.0f / (kLevels - 1), g * 255.0f / (kLevels - 1),
                                      b * 255.0f / (kLevels - 1)};
                float lms[3], seenLms[3], seen[3];
                apply(kRgbToLms, rgb, lms);
                apply(simulate, lms, seenLms);
                apply(kLmsToRgb, seenLms, seen);
                // The lost difference, shifted into the channels the deficiency keeps.
                const float error[3] = {rgb[0] - seen[0], rgb[1] - seen[1], rgb[2] - seen[2]};
                const float shift[3] = {0.0f, error[0] * 0.7f + error[1], error[0] * 0.7f + error[2]};
                for (int c = 0; c < 3; ++c)
                    g_table[at++] = static_cast<std::uint8_t>(std::clamp(rgb[c] + shift[c], 0.0f, 255.0f) + 0.5f);
            }
}

void colorVisionActivated()
{
    const char* mode = gba_mod_option_value(kPackageId, kFeatureId, "type");
    g_mode = mode && !std::strcmp(mode, "protan")   ? Mode::Protan
             : mode && !std::strcmp(mode, "tritan") ? Mode::Tritan
                                                    : Mode::Deutan;
    if (const char* env = std::getenv("ROTK_PC_COLOR_VISION"))
        g_mode = !std::strcmp(env, "protan") ? Mode::Protan : !std::strcmp(env, "tritan") ? Mode::Tritan : Mode::Deutan;
    buildTable(g_mode);
}

void colorVisionReset()
{
    g_mode = Mode::Off;
    if (std::getenv("ROTK_PC_COLOR_VISION")) colorVisionActivated();
}

}  // namespace

bool colorVisionActive()
{
    return g_mode != Mode::Off;
}

void colorVisionDecorate(const gbarecomp::PresentFrame* frame)
{
    if (g_mode == Mode::Off) return;
    constexpr int kShift = 2;  // 256 levels down to kLevels
    for (unsigned y = 0; y < frame->height; ++y) {
        std::uint8_t* pixel = frame->rgb + std::size_t(y) * frame->stride * 3u;
        for (unsigned x = 0; x < frame->width; ++x, pixel += 3) {
            const std::uint8_t* out = &g_table[(((std::size_t(pixel[0] >> kShift) * kLevels) +
                                                  (pixel[1] >> kShift)) * kLevels + (pixel[2] >> kShift)) * 3u];
            pixel[0] = out[0];
            pixel[1] = out[1];
            pixel[2] = out[2];
        }
    }
}

void colorVisionRegister()
{
    gba_mod_register_activation_plugin("rotk.pc.color-vision", colorVisionActivated);
    gba_mod_register_reset_callback(colorVisionReset);
}

}  // namespace rotk
