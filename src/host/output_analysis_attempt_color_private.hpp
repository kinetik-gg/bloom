#pragma once

// Private PNG output-colour resolution helper for OutputAnalysisAttemptRunnerV1. It owns the
// attempt graph's step-4 colour work: resolving the built-in config/working space, retaining the
// qualified CPU display-processor products, and preparing the immutable GPU DisplayRgba8 command
// from the exact resolved processor. Kept in its own translation unit so the runner's stage
// orchestration stays cohesive and within the owning-file size budget.

#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>

#include <memory>
#include <optional>

namespace bloom::host::detail {

// The PNG-only color half of the blocking stage, in the exact closed input vocabulary
// analyzePngRgba8SrgbV1 accepts. `display` is populated only when both fields below are the
// nominal Ready/Qualified pair.
struct ColorResolutionOutcomeV1 final {
    output::PngRgba8SrgbColorResolutionStateV1 colorResolution =
        output::PngRgba8SrgbColorResolutionStateV1::Ready;
    output::OutputAnalysisAdapterStateV1 adapter = output::OutputAnalysisAdapterStateV1::Qualified;
    output::OutputAnalysisAttemptDisplayProductsV1 display;
    // The already-compiled, immutable GPU DisplayRgba8 command prepared on this blocking CPU stage
    // from the exact resolved display processor, when qualified tools are available. Null keeps the
    // unchanged CPU display path (the truthful fallback).
    std::shared_ptr<const runtime::PreparedGpuOcioCommand> gpuDisplayCommand;
};

// The attempt graph's step 4. Never throws; a genuine allocation/internal failure is signalled by
// returning nullopt so the caller can fail the attempt AT the ColorPreparing stage, while every
// modelled configuration/adapter state returns a populated outcome that the analyzer turns into a
// truthful, non-approvable report.
[[nodiscard]] std::optional<ColorResolutionOutcomeV1> resolvePngDisplayProducts(
    runtime::QualifiedDisplayProcessorProvider* provider, GpuExportProvider* gpuProvider,
    const runtime::EvaluationColorIntent& intent, runtime::GpuOcioCommandGeometry geometry,
    const runtime::GpuOcioCancellation& cancel) noexcept;

} // namespace bloom::host::detail
