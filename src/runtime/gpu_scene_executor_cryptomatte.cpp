// CryptomatteCoverageV1 execution for GpuSceneExecutor (#364): validate a Cryptomatte step and
// dispatch one coverage pass over the resident rank stack on the device owner thread, then hand
// the resident matte back. Only the selected IDs leave the host; the pixels stay on the device.

#include <bloom/runtime/gpu_scene_executor.hpp>

#include "gpu_scene_executor_private.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::diagnosticFromCryptomatteCoverage;
using gpu_scene_executor_detail::makeDiagnostic;

} // namespace

GpuSceneExecutorDiagnostic
GpuSceneExecutor::Impl::startCryptomatteCoverageStep(const GpuSceneExecutorStep& step) {
    if (cryptomatteCoverage == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              "the Cryptomatte pipeline is not initialized");
    }
    if (step.input == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(step.input) >= images.size() || images[step.input] == nullptr ||
        !step.cryptomatteOutput.has_value()) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                              "a Cryptomatte step is incomplete before dispatch");
    }
    const render::GpuCryptomatteCoverageRequest parameters{
        .source = images[step.input],
        .output = *step.cryptomatteOutput,
        .rankCount = step.cryptomatteRankCount,
        .ids = step.cryptomatteIds,
    };
    const auto native = cryptomatteCoverage->begin(parameters, remainingBudget());
    if (native.code != render::GpuCryptomatteCoverageDiagnosticCode::None) {
        return diagnosticFromCryptomatteCoverage(native);
    }
    ++counters.cryptomatteDispatches;
    return {};
}

std::optional<render::GpuImage> GpuSceneExecutor::Impl::takeCryptomatteCoverageOutput() {
    if (cryptomatteCoverage == nullptr) {
        return std::nullopt;
    }
    auto output = cryptomatteCoverage->take();
    if (!output.isValid()) {
        return std::nullopt;
    }
    return std::optional<render::GpuImage>(std::move(output));
}

bool GpuSceneExecutor::Impl::cryptomatteCoverageDeviceLost() const noexcept {
    return cryptomatteCoverage != nullptr &&
           cryptomatteCoverage->diagnostic().code ==
               render::GpuCryptomatteCoverageDiagnosticCode::DeviceLost;
}

bool GpuSceneExecutor::Impl::cryptomatteCoverageCancelled() const noexcept {
    return cryptomatteCoverage != nullptr &&
           cryptomatteCoverage->diagnostic().code ==
               render::GpuCryptomatteCoverageDiagnosticCode::Cancelled;
}

void GpuSceneExecutor::Impl::cancelCryptomatteCoverage() noexcept {
    if (cryptomatteCoverage != nullptr) {
        cryptomatteCoverage->cancel();
    }
}

void GpuSceneExecutor::Impl::discardCryptomatteCoverageOutput() noexcept {
    if (cryptomatteCoverage != nullptr) {
        static_cast<void>(cryptomatteCoverage->take());
    }
}

} // namespace bloom::runtime
