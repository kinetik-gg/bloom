#pragma once
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/render/image.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace bloom::color {
class CpuColorSpaceProcessor;
}
namespace bloom::media::video {
struct VideoInputColorSpaceResolution final {
    std::string id;
    std::string name;
    std::string warning;
    bool noConversion = false;
    bool automatic = false;
};

// Maps H.273 container tags, or a legacy/explicit asset choice, to an exact non-data colour-space
// id in the qualified config. Pixel conversion remains the Rec.709 YUV path below.
[[nodiscard]] VideoInputColorSpaceResolution
resolveVideoInputColorSpace(const bloom::color::ResolvedBloomNeutralConfig& config,
                            const media::provider::ColourTags& tags, std::uint32_t legacyColorSpace,
                            std::string_view explicitId);

// H.273 Rec.709 matrix, limited/full range; Rec.709 or sRGB inverse transfer.
// Override: 0 stream tags, 1 sRGB, 2 linear, 3 raw. HDR/wide gamut remain unavailable.
[[nodiscard]] media::provider::Result<render::Rgba32fImage>
videoToSceneLinear(const media::provider::FrameProduct& frame, std::uint32_t overrideTransfer,
                   render::Rgba32fImageDescriptor composition, double horizontalScale,
                   double verticalScale, std::size_t byteBudget,
                   const std::function<bool()>& cancel = {});
[[nodiscard]] media::provider::Result<render::Rgba32fImage>
videoToSceneLinear(const media::provider::FrameProduct& frame, std::uint32_t overrideTransfer,
                   std::string_view inputColorSpaceId,
                   const std::shared_ptr<const bloom::color::CpuColorSpaceProcessor>& processor,
                   render::Rgba32fImageDescriptor composition, double horizontalScale,
                   double verticalScale, std::size_t byteBudget,
                   const std::function<bool()>& cancel = {});
} // namespace bloom::media::video
