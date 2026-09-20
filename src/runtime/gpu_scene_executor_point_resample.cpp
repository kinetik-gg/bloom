// PointResampleV1 execution for GpuSceneExecutor: validate a point-resample step and dispatch one
// nearest-neighbour gather on the device owner thread, then hand the resident proxy output back.
// Kept in its own translation unit so gpu_scene_executor_execution.cpp stays cohesive and under the
// project's line budget. It never reads back a full frame and never resamples a pixel on the host:
// the immutable axis metadata is prepared inside GpuPointResample::begin; the pixels stay on the
// device.

#include <bloom/runtime/gpu_scene_executor.hpp>

#include "gpu_scene_executor_private.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::diagnosticFromPointResample;
using gpu_scene_executor_detail::makeDiagnostic;

} // namespace

GpuSceneExecutorDiagnostic
GpuSceneExecutor::Impl::startPointResampleStep(const GpuSceneExecutorStep& step) {
    if (pointResample == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              "the point-resample pipeline is not initialized");
    }
    if (step.input == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(step.input) >= images.size() || images[step.input] == nullptr ||
        !step.pointResampleOutput.has_value()) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                              "a point-resample step is incomplete before dispatch");
    }
    const render::GpuPointResampleRequest parameters{
        .source = images[step.input],
        .output = *step.pointResampleOutput,
        .horizontalScale = step.pointResampleHorizontalScale,
        .verticalScale = step.pointResampleVerticalScale,
    };
    const auto native = pointResample->begin(parameters, remainingBudget());
    if (native.code != render::GpuPointResampleDiagnosticCode::None) {
        return diagnosticFromPointResample(native);
    }
    ++counters.pointResampleDispatches;
    return {};
}

std::optional<render::GpuImage> GpuSceneExecutor::Impl::takePointResampleOutput() {
    if (pointResample == nullptr) {
        return std::nullopt;
    }
    auto output = pointResample->take();
    if (!output.isValid()) {
        return std::nullopt;
    }
    return std::optional<render::GpuImage>(std::move(output));
}

bool GpuSceneExecutor::Impl::pointResampleDeviceLost() const noexcept {
    return pointResample != nullptr &&
           pointResample->diagnostic().code == render::GpuPointResampleDiagnosticCode::DeviceLost;
}

bool GpuSceneExecutor::Impl::pointResampleCancelled() const noexcept {
    return pointResample != nullptr &&
           pointResample->diagnostic().code == render::GpuPointResampleDiagnosticCode::Cancelled;
}

void GpuSceneExecutor::Impl::cancelPointResample() noexcept {
    if (pointResample != nullptr) {
        pointResample->cancel();
    }
}

void GpuSceneExecutor::Impl::discardPointResampleOutput() noexcept {
    if (pointResample != nullptr) {
        static_cast<void>(pointResample->take());
    }
}

} // namespace bloom::runtime
