#include "output_analysis_attempt_color_private.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>

#include <new>
#include <optional>
#include <utility>

namespace bloom::host::detail {
namespace {

// Maps the C2 in-process registry's own closed outcome set onto the analyzer's closed PNG
// color-resolution input states, one to one, per frame-output.md's "The five ocio.* codes map
// one-to-one from the corresponding typed color-resolution failures". `Ready` is handled by the
// caller (it is the only outcome that carries a resolved product to build a processor from).
[[nodiscard]] output::PngRgba8SrgbColorResolutionStateV1
mapRegistryOutcome(const color::OcioBuiltInRegistryOutcome outcome) noexcept {
    switch (outcome) {
    case color::OcioBuiltInRegistryOutcome::Ready:
        break; // unreachable here; the caller only maps a non-Ready outcome.
    case color::OcioBuiltInRegistryOutcome::Missing:
        return output::PngRgba8SrgbColorResolutionStateV1::Missing;
    case color::OcioBuiltInRegistryOutcome::Changed:
        return output::PngRgba8SrgbColorResolutionStateV1::Changed;
    case color::OcioBuiltInRegistryOutcome::Invalid:
        return output::PngRgba8SrgbColorResolutionStateV1::Invalid;
    case color::OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper:
        return output::PngRgba8SrgbColorResolutionStateV1::MissingResource;
    }
    return output::PngRgba8SrgbColorResolutionStateV1::Missing;
}

// Binds a built or reused processor handle to the retained display-product triple. The identity is
// an ALIASING shared_ptr into the handle's own DisplayProcessorIdentityV1 member -- never an
// independently adopted copy -- so the exported identity can never be paired with a different
// processor (frame-output.md: "Recomputing pixel hashes or substituting an equivalent-looking
// frame or processor at approval or export is forbidden").
[[nodiscard]] std::optional<output::OutputAnalysisAttemptDisplayProductsV1>
retainDisplayProducts(std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) {
    core::Sha256Digest expectedRevision;
    if (const auto view = handle->identity().borrowedView(); view.has_value()) {
        expectedRevision = view->expectedOcioRevision();
    } else {
        return std::nullopt;
    }
    std::shared_ptr<const color::DisplayProcessorIdentityV1> identity(handle, &handle->identity());
    return output::OutputAnalysisAttemptDisplayProductsV1{.processor = std::move(handle),
                                                          .identity = std::move(identity),
                                                          .expectedOcioRevision = expectedRevision};
}

// Prepares the immutable GPU DisplayRgba8 command from the exact resolved display processor, on
// this blocking CPU stage. Unavailable qualified tools or a preparation failure return null: the
// request then keeps the unchanged CPU display path. Never throws.
[[nodiscard]] std::shared_ptr<const runtime::PreparedGpuOcioCommand>
prepareGpuDisplayCommand(GpuExportProvider* const gpuProvider,
                         const color::ResolvedBloomNeutralConfig& config,
                         const runtime::GpuOcioCommandGeometry geometry,
                         const runtime::GpuOcioCancellation& cancel) noexcept {
    try {
        if (gpuProvider == nullptr || !gpuProvider->gpuDisplayPreparationAvailable()) {
            return nullptr;
        }
        auto prepared = gpuProvider->prepareGpuDisplayCommand(config, config.displayName(),
                                                              config.viewName(), geometry, cancel);
        return prepared.hasValue() ? prepared.command : nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace

std::optional<ColorResolutionOutcomeV1>
resolvePngDisplayProducts(runtime::QualifiedDisplayProcessorProvider* const provider,
                          GpuExportProvider* const gpuProvider,
                          const runtime::EvaluationColorIntent& intent,
                          const runtime::GpuOcioCommandGeometry geometry,
                          const runtime::GpuOcioCancellation& cancel) noexcept {
    try {
        const bool neutralWorkingSpace =
            intent.workingColorSpaceId == runtime::kLinearRec709SceneColorSpaceId &&
            (intent.ocioConfigRevision == core::Sha256Digest{} ||
             intent.ocioConfigRevision == color::kBloomNeutralV1ConfigDigest);
        const auto expectedRevision = intent.ocioConfigRevision == core::Sha256Digest{}
                                          ? color::kBloomNeutralV1ConfigDigest
                                          : intent.ocioConfigRevision;
        if (neutralWorkingSpace && provider != nullptr) {
            const auto snapshot = provider->snapshot();
            if (snapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Ready &&
                snapshot.handle != nullptr) {
                auto products = retainDisplayProducts(snapshot.handle);
                if (!products.has_value()) {
                    return std::nullopt;
                }
                // Prepare the GPU command from an independent resolution of the same exact config
                // (the ready CPU handle does not expose its config). A failure here is silent: the
                // CPU display path stays the truthful fallback.
                std::shared_ptr<const runtime::PreparedGpuOcioCommand> gpuCommand;
                auto gpuResolution = color::resolveOcioBuiltIn(
                    color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                    expectedRevision, intent.workingColorSpaceId);
                if (gpuResolution.ready()) {
                    if (auto gpuConfig = std::move(gpuResolution).takeResolved();
                        gpuConfig.has_value()) {
                        gpuCommand =
                            prepareGpuDisplayCommand(gpuProvider, *gpuConfig, geometry, cancel);
                    }
                }
                return ColorResolutionOutcomeV1{
                    .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready,
                    .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                    .display = std::move(*products),
                    .gpuDisplayCommand = std::move(gpuCommand)};
            }
        }

        const auto locator =
            neutralWorkingSpace ? color::kBloomNeutralV1ConfigUri : color::kAcesCgV1ConfigUri;
        auto resolution =
            color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, locator,
                                      expectedRevision, intent.workingColorSpaceId);
        if (!resolution.ready()) {
            return ColorResolutionOutcomeV1{
                .colorResolution = mapRegistryOutcome(resolution.outcome()),
                .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                .display = {},
                .gpuDisplayCommand = {}};
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            return std::nullopt;
        }

        // The GPU display command is prepared for EVERY built-in config/working/display the CPU
        // resolver supports, not just Bloom Neutral: the same exact resolved display processor the
        // CPU path uses. Unavailable tools or a preparation failure return null and keep the CPU
        // display path as the truthful fallback.
        auto gpuCommand = prepareGpuDisplayCommand(gpuProvider, *resolved, geometry, cancel);

        // frame-output.md: "A resolved PNG configuration whose helper, processor, or execution
        // provider cannot run is an adapter-execution failure: Color keeps its Ready nominal tuple
        // while External Dependencies uses adapter.unavailable."
        auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        auto handleValue = std::move(built).takeHandle();
        if (!handleValue.has_value()) {
            return ColorResolutionOutcomeV1{
                .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready,
                .adapter = output::OutputAnalysisAdapterStateV1::Unavailable,
                .display = {},
                .gpuDisplayCommand = {}};
        }
        auto handle = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handleValue));
        auto products = retainDisplayProducts(std::move(handle));
        if (!products.has_value()) {
            return std::nullopt;
        }
        return ColorResolutionOutcomeV1{.colorResolution =
                                            output::PngRgba8SrgbColorResolutionStateV1::Ready,
                                        .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                                        .display = std::move(*products),
                                        .gpuDisplayCommand = std::move(gpuCommand)};
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace bloom::host::detail
