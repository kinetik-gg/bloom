#pragma once
#include <array>
#include <cstdint>
namespace bloom::color {
// H.273 Rec.709 matrix; samples are in 8-bit code units, transfer 1=709,13=sRGB,8=linear.
[[nodiscard]] std::array<float, 3> rec709YuvToSceneLinear(float luma, float cb, float cr,
                                                          bool limited, std::int32_t transfer);
// Any supported H.273 YUV matrix: 1 BT.709, 5/6 BT.601, 9 BT.2020 non-constant luminance.
// BT.709 is bit-identical to rec709YuvToSceneLinear; other codes return NaN.
[[nodiscard]] std::array<float, 3> yuvToSceneLinear(float luma, float cb, float cr, bool limited,
                                                    std::int32_t transfer, std::int32_t matrix);
} // namespace bloom::color
