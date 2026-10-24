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

// Owner decision (2026-10-24): an untagged or partly tagged SDR stream is not refused. Each
// missing tag is filled by the FFmpeg/NLE convention: Rec.709 for pictures 1280 wide or taller
// than 576 lines, Rec.601 below (BT.470BG at 25/50 fps, SMPTE 170M otherwise), the BT.709 curve
// and limited range. A missing matrix follows BT.2020 non-constant luminance when the primaries are BT.2020 or
// the transfer is PQ/HLG. Present tags are never changed.
struct VideoGeometry final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frameRate = 0.0;
};
struct AssumedVideoColour final {
    media::provider::ColourTags tags;
    bool assumed = false;
    // Visible note naming the assumption and where to change it; empty when nothing was assumed.
    std::string note;
    // Part of cache and transform identity ("" when nothing was assumed).
    std::string identity;
};
[[nodiscard]] AssumedVideoColour assumeMissingVideoColour(const media::provider::ColourTags& tags,
                                                          const VideoGeometry& geometry);
// Resolution with the assumption applied first; a successful assumed resolution carries the note
// as its warning so Assets and the Viewer show it.
[[nodiscard]] VideoInputColorSpaceResolution
resolveVideoInputColorSpace(const bloom::color::ResolvedBloomNeutralConfig& config,
                            const media::provider::ColourTags& tags, std::uint32_t legacyColorSpace,
                            std::string_view explicitId, const VideoGeometry& geometry);

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

// Codec-side preparation ONLY: the same H.273 Rec.709 YUV matrix, range handling and
// config-managed transfer-8 (no curve) path the config-managed videoToSceneLinear overload uses,
// but with NO OCIO processor applied. It produces premultiplied RGB in the source's pre-OCIO input
// state at the requested resolution. The caller emits the input->working OCIO transform as a GPU
// command over this image, so no OCIO colour pass is hidden in host code.
//
// `inputColorSpaceId` must be non-empty: it selects the config-managed transfer path exactly as the
// CPU reference does, so the host bytes match the CPU's pre-OCIO state byte for byte.
[[nodiscard]] media::provider::Result<render::Rgba32fImage>
videoToInputColorSpace(const media::provider::FrameProduct& frame, std::uint32_t overrideTransfer,
                       std::string_view inputColorSpaceId,
                       render::Rgba32fImageDescriptor composition, double horizontalScale,
                       double verticalScale, std::size_t byteBudget,
                       const std::function<bool()>& cancel = {});
} // namespace bloom::media::video
