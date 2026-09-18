#pragma once
#include <array>
#include <cstdint>
namespace bloom::color {
// H.273 Rec.709 matrix; samples are in 8-bit code units, transfer 1=709,13=sRGB,8=linear.
[[nodiscard]] std::array<float, 3> rec709YuvToSceneLinear(float luma, float cb, float cr,
                                                          bool limited, std::int32_t transfer);
} // namespace bloom::color
