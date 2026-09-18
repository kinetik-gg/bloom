#include <bloom/color/ocio_cpu_color_space_processor.hpp>

#include "ocio_internal.hpp"
#include <bloom/core/floating_point.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace bloom::color {

namespace {
inline constexpr std::size_t kMaxProcessorColorSpaceIdBytes = 256;

[[nodiscard]] bool validId(const std::string_view id) noexcept {
    return !id.empty() && id.size() <= kMaxProcessorColorSpaceIdBytes &&
           id.find('\0') == std::string_view::npos;
}
} // namespace

class CpuColorSpaceProcessor::Impl final {
  public:
    explicit Impl(OCIO::ConstCPUProcessorRcPtr processor) noexcept
        : processor_(std::move(processor)) {}

    OCIO::ConstCPUProcessorRcPtr processor_;
};

CpuColorSpaceProcessor::CpuColorSpaceProcessor(std::unique_ptr<Impl> impl,
                                               const core::Sha256Digest revision,
                                               std::string fromId, std::string workingSpaceId,
                                               const bool identity) noexcept
    : impl_(std::move(impl)), revision_(revision), fromId_(std::move(fromId)),
      workingSpaceId_(std::move(workingSpaceId)), identity_(identity) {}

CpuColorSpaceProcessor::~CpuColorSpaceProcessor() = default;

OcioColorSpaceProcessorResult
CpuColorSpaceProcessor::prepare(const ResolvedBloomNeutralConfig& config,
                                const std::string_view fromId,
                                const std::string_view toWorkingSpaceId) noexcept {
    if (!validId(fromId) || !validId(toWorkingSpaceId)) {
        return OcioColorSpaceProcessorResult(
            {}, !validId(fromId) ? OcioColorSpaceProcessorError::InvalidColorSpaceId
                                 : OcioColorSpaceProcessorError::MissingWorkingColorSpace);
    }
    const auto& ocioConfig = config.impl().config();
    const std::string from(fromId);
    const std::string working(toWorkingSpaceId);
    try {
        const auto input = ocioConfig->getColorSpace(from.c_str());
        if (!input)
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::MissingInputColorSpace);
        if (input->isData())
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::InputColorSpaceIsData);
        const auto target = ocioConfig->getColorSpace(working.c_str());
        if (!target)
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::MissingWorkingColorSpace);
        if (target->isData() || target->getReferenceSpaceType() != OCIO::REFERENCE_SPACE_SCENE ||
            !ocioConfig->isColorSpaceLinear(working.c_str(), OCIO::REFERENCE_SPACE_SCENE)) {
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::WorkingColorSpaceInvalid);
        }
        if (from == working) {
            auto result = std::shared_ptr<const CpuColorSpaceProcessor>(
                new CpuColorSpaceProcessor({}, config.expectedRevision(), from, working, true));
            return OcioColorSpaceProcessorResult(std::move(result),
                                                 OcioColorSpaceProcessorError::None);
        }
        if (!core::supportsReferenceFloatingPointEnvironment<float>()) {
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::UnsupportedFloatingPointEnvironment);
        }
        const auto context = OCIO::Context::Create();
        const auto processor = ocioConfig->getProcessor(context, from.c_str(), working.c_str());
        if (!processor)
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::TransformBuildFailed);
        const auto cpu = processor->getDefaultCPUProcessor();
        if (!cpu)
            return OcioColorSpaceProcessorResult(
                {}, OcioColorSpaceProcessorError::CpuProcessorUnavailable);
        auto result = std::shared_ptr<const CpuColorSpaceProcessor>(new CpuColorSpaceProcessor(
            std::make_unique<Impl>(cpu), config.expectedRevision(), from, working, false));
        return OcioColorSpaceProcessorResult(std::move(result), OcioColorSpaceProcessorError::None);
    } catch (const std::exception&) {
        return OcioColorSpaceProcessorResult({},
                                             OcioColorSpaceProcessorError::TransformBuildFailed);
    }
}

bool CpuColorSpaceProcessor::apply(std::span<std::array<float, 4>> pixels) const noexcept {
    if (!core::supportsReferenceFloatingPointEnvironment<float>())
        return false;
    try {
        for (auto& pixel : pixels) {
            if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1]) || !std::isfinite(pixel[2]))
                return false;
            if (!identity_)
                impl_->processor_->applyRGB(pixel.data());
            if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1]) || !std::isfinite(pixel[2]))
                return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace bloom::color
