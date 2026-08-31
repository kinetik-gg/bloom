#include <bloom/color/ocio_cpu_display_processor.hpp>

#include "ocio_internal.hpp"
#include <bloom/color/display_processor_identity.hpp>

#include <array>
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
buildIdentity(const ResolvedBloomNeutralConfig& resolved) {
    const DisplayProcessorIdentityV1InputView input{
        .expectedOcioRevision = resolved.expectedRevision(),
        .contextVariables = {},
        .sourceColorSpaceId = resolved.processColorSpaceId(),
        .displayName = resolved.displayName(),
        .viewName = resolved.viewName(),
        .lookMode = DisplayProcessorLookModeV1::Bypass,
        .lookNames = {},
        .outputColorSpaceId = resolved.outputColorSpaceId(),
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

} // namespace

namespace bloom::color {

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
    const auto& config = resolved.impl().config();

    OCIO::ConstProcessorRcPtr processor;
    OCIO::ConstCPUProcessorRcPtr cpuProcessor;
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
        processor = config->getProcessor(
            emptyContext, std::string(resolved.processColorSpaceId()).c_str(),
            std::string(resolved.displayName()).c_str(), std::string(resolved.viewName()).c_str(),
            OCIO::TRANSFORM_DIR_FORWARD);
        if (!processor) {
            return OcioBuildProcessorResult(OcioBuildProcessorError::GetProcessorFailed);
        }
        cacheId = processor->getCacheID();
        cpuProcessor = processor->getDefaultCPUProcessor();
        if (!cpuProcessor) {
            return OcioBuildProcessorResult(OcioBuildProcessorError::GetCpuProcessorFailed);
        }
    } catch (const OCIO::Exception&) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::GetProcessorFailed);
    } catch (const std::exception&) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::GetProcessorFailed);
    }

    auto identity = buildIdentity(resolved);
    if (!identity.has_value()) {
        return OcioBuildProcessorResult(OcioBuildProcessorError::IdentityConstructionFailed);
    }

    DisplayProcessorExecutionProvenance provenance{
        .ocioVersion = ocioVersionString(),
        .compilerId = compilerIdString(),
        .compilerVersion = compilerVersionString(),
        .targetTriple = BLOOM_COLOR_OCIO_TARGET_TRIPLE,
        .processorCacheId = cacheId,
        .dependencyLockDigest = std::nullopt,
        .qualifiedPrefixDigest = std::nullopt,
    };

    auto impl = std::make_unique<PreparedCpuDisplayProcessorHandle::Impl>(cpuProcessor);
    PreparedCpuDisplayProcessorHandle handle(std::move(impl), std::move(*identity),
                                             std::move(provenance),
                                             DisplayProcessorLease::inProcess());
    return OcioBuildProcessorResult(std::move(handle));
}

} // namespace bloom::color
