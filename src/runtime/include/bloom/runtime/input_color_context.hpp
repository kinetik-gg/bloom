#pragma once

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
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

} // namespace bloom::runtime::detail
