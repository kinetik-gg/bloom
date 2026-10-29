#pragma once

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime::detail {

struct InputColorSpaceResolution final {
    std::string id;
    std::string name;
    std::string warning;
    bool noConversion = false;
    bool automatic = false;
};

[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
resolveInputColorConfig(const EvaluationColorIntent& intent);

[[nodiscard]] InputColorSpaceResolution
resolveImageInputColorSpace(const color::ResolvedBloomNeutralConfig& config,
                            const media::ImageProbe& probe, media::ImageColorSpace legacy,
                            std::string_view explicitId);

[[nodiscard]] std::shared_ptr<const color::CpuColorSpaceProcessor>
prepareInputColorProcessor(const color::ResolvedBloomNeutralConfig& config,
                           const InputColorSpaceResolution& resolution, std::string& diagnostic);

// ADR 0032 compatibility: true for a video asset whose input colour is Auto (no explicit config
// space and no legacy choice) and whose captured stream tags make Auto show it as ordinary SDR
// video through the sRGB texture space. Projects saved before document 1.28 showed exactly these
// assets through the Rec.709 video space, so opening one shows a one-time note.
[[nodiscard]] bool assetUsesSdrVideoColourDefault(const document::AssetRecord& asset);

} // namespace bloom::runtime::detail
