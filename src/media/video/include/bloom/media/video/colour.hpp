#pragma once
#include <bloom/media/provider/contract.hpp>
#include <bloom/render/image.hpp>
#include <functional>
namespace bloom::media::video {
// H.273 Rec.709 matrix, limited/full range; Rec.709 or sRGB inverse transfer.
// Override: 0 stream tags, 1 sRGB, 2 linear, 3 raw. HDR/wide gamut remain unavailable.
[[nodiscard]] media::provider::Result<render::Rgba32fImage>
videoToSceneLinear(const media::provider::FrameProduct& frame, std::uint32_t overrideTransfer,
                   render::Rgba32fImageDescriptor composition, double horizontalScale,
                   double verticalScale, std::size_t byteBudget,
                   const std::function<bool()>& cancel = {});
} // namespace bloom::media::video
