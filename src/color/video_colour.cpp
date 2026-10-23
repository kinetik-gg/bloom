#include <bloom/color/video_colour.hpp>
#include <cmath>
#include <limits>
#include <utility>
namespace bloom::color {
namespace {
float inverse(float value, std::int32_t transfer) {
    if (transfer == 8)
        return value;
    if (transfer == 13)
        return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
}
} // namespace
std::array<float, 3> rec709YuvToSceneLinear(float luma, float cb, float cr, bool limited,
                                            std::int32_t transfer) {
    const auto yy = limited ? (luma - 16.0F) / 219.0F : luma / 255.0F;
    const auto u = (cb - 128.0F) / (limited ? 224.0F : 255.0F);
    const auto v = (cr - 128.0F) / (limited ? 224.0F : 255.0F);
    return {inverse(yy + 1.5748F * v, transfer),
            inverse(yy - 0.18732427F * u - 0.46812427F * v, transfer),
            inverse(yy + 1.8556F * u, transfer)};
}
std::array<float, 3> yuvToSceneLinear(float luma, float cb, float cr, bool limited,
                                      std::int32_t transfer, std::int32_t matrix) {
    if (matrix == 1)
        return rec709YuvToSceneLinear(luma, cb, cr, limited, transfer);
    const auto [kr, kb] = matrix == 9                  ? std::pair{0.2627F, 0.0593F}
                          : matrix == 5 || matrix == 6 ? std::pair{0.299F, 0.114F}
                                                       : std::pair{-1.0F, -1.0F};
    if (kr < 0) {
        const auto invalid = std::numeric_limits<float>::quiet_NaN();
        return {invalid, invalid, invalid};
    }
    const auto kg = 1.0F - kr - kb;
    const auto yy = limited ? (luma - 16.0F) / 219.0F : luma / 255.0F;
    const auto u = (cb - 128.0F) / (limited ? 224.0F : 255.0F);
    const auto v = (cr - 128.0F) / (limited ? 224.0F : 255.0F);
    return {
        inverse(yy + 2.0F * (1.0F - kr) * v, transfer),
        inverse(yy - 2.0F * kb * (1.0F - kb) / kg * u - 2.0F * kr * (1.0F - kr) / kg * v, transfer),
        inverse(yy + 2.0F * (1.0F - kb) * u, transfer)};
}
} // namespace bloom::color
