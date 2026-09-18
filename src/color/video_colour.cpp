#include <bloom/color/video_colour.hpp>
#include <cmath>
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
} // namespace bloom::color
