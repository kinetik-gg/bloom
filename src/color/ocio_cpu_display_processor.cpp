#include <bloom/color/ocio_cpu_display_processor.hpp>

#include "ocio_internal.hpp"
#include <bloom/color/display_processor_identity.hpp>

#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using bloom::color::DisplayProcessorContextVariableV1View;
using bloom::color::DisplayProcessorIdentityV1InputView;
using bloom::color::DisplayProcessorLookModeV1;
using bloom::color::ResolvedBloomNeutralConfig;

// Builds the version 1 canonical DisplayProcessorIdentity for the Bloom Neutral request: empty
// context, look bypass, quality "reference", packing "straight-rgba8", the frozen semantics
// profile "bloom.color.ocio-cpu-display.v1", and the source/output Color Interop IDs and
// display/view names discovered by ResolvedBloomNeutralConfig (per docs/architecture/
// color-management.md's "Qualified Display Intent And Identity").
[[nodiscard]] std::optional<bloom::color::DisplayProcessorIdentityV1>
buildIdentity(const ResolvedBloomNeutralConfig& resolved, const std::string_view displayName,
              const std::string_view viewName, const std::string_view outputColorSpaceId) {
    const DisplayProcessorIdentityV1InputView input{
        .expectedOcioRevision = resolved.expectedRevision(),
        .contextVariables = {},
        .sourceColorSpaceId = resolved.processColorSpaceId(),
        .displayName = displayName,
        .viewName = viewName,
        .lookMode = DisplayProcessorLookModeV1::Bypass,
        .lookNames = {},
        .outputColorSpaceId = outputColorSpaceId,
        .qualityId = bloom::color::kDisplayProcessorIdentityQualityId,
        .semanticsProfileId = bloom::color::kDisplayProcessorIdentitySemanticsProfileId,
        .packingId = bloom::color::kDisplayProcessorIdentityPackingId,
    };

    const auto validation = bloom::color::validateDisplayProcessorIdentityV1(input);
    if (!validation) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(validation.requiredByteCount());
    const auto writeResult = bloom::color::writeDisplayProcessorIdentityV1(input, bytes);
    if (!writeResult) {
        return std::nullopt;
    }
    auto adoption = bloom::color::adoptDisplayProcessorIdentityV1(std::move(bytes));
    if (!adoption) {
        return std::nullopt;
    }
    return std::move(adoption).takeIdentity();
}

[[nodiscard]] std::string ocioVersionString() { return OCIO::GetVersion(); }

[[nodiscard]] std::string compilerIdString() {
#if defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "Unknown";
#endif
}

[[nodiscard]] std::string compilerVersionString() {
#if defined(__clang__)
    return __clang_version__;
#elif defined(__GNUC__)
    return std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return std::to_string(_MSC_VER);
#else
    return {};
#endif
}

[[nodiscard]] std::optional<bloom::core::Color4d>
convertColor(const OCIO::ConstCPUProcessorRcPtr& processor, bloom::core::Color4d value,
             const bool clampDisplay) noexcept {
    if (!value.isValid() || !bloom::core::supportsReferenceFloatingPointEnvironment<float>() ||
        !bloom::core::supportsReferenceFloatingPointEnvironment<double>()) {
        return std::nullopt;
    }
    const std::array channels{value.red, value.green, value.blue};
    std::array<float, 3> rgb{};
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        if (std::abs(channels[i]) > static_cast<double>(std::numeric_limits<float>::max()))
            return std::nullopt;
        rgb[i] = static_cast<float>(channels[i]);
    }
    try {
        processor->applyRGB(rgb.data());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    for (auto& channel : rgb) {
        if (!std::isfinite(channel))
            return std::nullopt;
        if (clampDisplay)
            channel = std::clamp(channel, 0.0F, 1.0F);
    }
    return bloom::core::Color4d{static_cast<double>(rgb[0]), static_cast<double>(rgb[1]),
                                static_cast<double>(rgb[2]), value.alpha};
}

} // namespace

namespace bloom::color {

std::optional<core::Color4d>
PreparedCpuDisplayProcessorHandle::referenceToDisplay(const core::Color4d value) const noexcept {
    return convertColor(impl_->cpuProcessor(), value, true);
}

std::optional<core::Color4d> PreparedCpuDisplayProcessorHandle::referenceToDisplayLinear(
    const core::Color4d value) const noexcept {
    return convertColor(impl_->cpuProcessor(), value, false);
}

std::optional<core::Color4d>
PreparedCpuDisplayProcessorHandle::displayToReference(const core::Color4d value) const noexcept {
    return convertColor(impl_->inverseProcessor(), value, false);
}

PreparedCpuDisplayProcessorHandle::PreparedCpuDisplayProcessorHandle(
    std::unique_ptr<Impl> impl, DisplayProcessorIdentityV1 identity,
    DisplayProcessorExecutionProvenance provenance, DisplayProcessorLease lease) noexcept
    : impl_(std::move(impl)), identity_(std::move(identity)), provenance_(std::move(provenance)),
      lease_(lease) {}

PreparedCpuDisplayProcessorHandle::PreparedCpuDisplayProcessorHandle(
    PreparedCpuDisplayProcessorHandle&&) noexcept = default;
PreparedCpuDisplayProcessorHandle::~PreparedCpuDisplayProcessorHandle() = default;

OcioBuildProcessorResult
buildBloomNeutralCpuDisplayProcessor(const ResolvedBloomNeutralConfig& resolved) noexcept {
    return buildBloomNeutralCpuDisplayProcessor(resolved, resolved.displayName(),
                                                resolved.viewName());
}

OcioBuildProcessorResult
buildBloomNeutralCpuDisplayProcessor(const ResolvedBloomNeutralConfig& resolved,
                                     const std::string_view displayName,
                                     const std::string_view viewName) noexcept {
    return buildCpuDisplayProcessorForView(resolved, displayName, viewName);
}

OcioBuildProcessorResult
buildCpuDisplayProcessorForView(const ResolvedBloomNeutralConfig& resolved,
                                const std::string_view displayName,
                                const std::string_view viewName) noexcept {
    const auto entry = std::find_if(
        resolved.displays().begin(), resolved.displays().end(), [&](const auto& candidate) {
            return candidate.display == displayName && candidate.view == viewName;
        });
    if (entry == resolved.displays().end()) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::DisplayViewNotFound);
    }
    const auto& config = resolved.impl().config();

    OCIO::ConstProcessorRcPtr processor;
    OCIO::ConstCPUProcessorRcPtr cpuProcessor;
    OCIO::ConstCPUProcessorRcPtr inverseProcessor;
    std::string cacheId;
    try {
        // A freshly created Context is passed explicitly rather than relying on
        // config->getCurrentContext(): OCIO auto-populates a config's default context from every
        // process environment variable (EnvironmentMode::ENV_ENVIRONMENT_LOAD_ALL is the
        // default), and Context::Create() below is never populated from the environment. This is
        // "no environment, working-directory, or search-path influence" for the actual transform
        // build, independent of what the config's own default context contains -- see
        // ocio_builtin_registry.cpp's resolution-time comment for the companion assertion that
        // the config declares no "environment:" section of its own.
        const OCIO::ConstContextRcPtr emptyContext = OCIO::Context::Create();
        auto transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(std::string(resolved.processColorSpaceId()).c_str());
        transform->setDisplay(std::string(displayName).c_str());
        transform->setView(std::string(viewName).c_str());
        processor = config->getProcessor(emptyContext, transform, OCIO::TRANSFORM_DIR_FORWARD);
        if (!processor) {
            return OcioBuildProcessorResult(OcioBuildProcessorError::GetProcessorFailed);
        }
        cacheId = processor->getCacheID();
        cpuProcessor = processor->getDefaultCPUProcessor();
        const auto inverse =
            config->getProcessor(emptyContext, transform, OCIO::TRANSFORM_DIR_INVERSE);
        // Authoring must not turn display white into an artificial HDR value through fast pow.
        inverseProcessor = inverse->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_LOSSLESS);
        if (!cpuProcessor || !inverseProcessor) {
            return OcioBuildProcessorResult(OcioBuildProcessorError::GetCpuProcessorFailed);
        }
    } catch (const OCIO::Exception&) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::GetProcessorFailed);
    } catch (const std::exception&) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::GetProcessorFailed);
    }

    auto identity = buildIdentity(resolved, displayName, viewName, entry->colourSpaceId);
    if (!identity.has_value()) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::IdentityConstructionFailed);
    }

    DisplayProcessorExecutionProvenance provenance{
        .ocioVersion = ocioVersionString(),
        .compilerId = compilerIdString(),
        .compilerVersion = compilerVersionString(),
        .targetTriple = BLOOM_COLOR_OCIO_TARGET_TRIPLE,
        .processorCacheId = cacheId,
        .displayName = std::string(displayName),
        .viewName = std::string(viewName),
        .dependencyLockDigest = std::nullopt,
        .qualifiedPrefixDigest = std::nullopt,
    };

    auto impl =
        std::make_unique<PreparedCpuDisplayProcessorHandle::Impl>(cpuProcessor, inverseProcessor);
    PreparedCpuDisplayProcessorHandle handle(std::move(impl), std::move(*identity),
                                             std::move(provenance),
                                             DisplayProcessorLease::inProcess());
    return OcioBuildProcessorResult(std::move(handle));
}

} // namespace bloom::color
