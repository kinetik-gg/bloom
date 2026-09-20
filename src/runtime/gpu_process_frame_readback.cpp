#include "gpu_process_frame_readback_private.hpp"

#include <cstring>
#include <thread>
#include <utility>

namespace bloom::runtime::detail {
namespace {

using namespace std::chrono_literals;

} // namespace

GpuProcessFrameDiagnosticCode
mapOutputColorCode(const GpuOutputColorDiagnosticCode code) noexcept {
    switch (code) {
    case GpuOutputColorDiagnosticCode::None:
        return GpuProcessFrameDiagnosticCode::None;
    case GpuOutputColorDiagnosticCode::InvalidArgument:
    case GpuOutputColorDiagnosticCode::GeometryMismatch:
        return GpuProcessFrameDiagnosticCode::InvalidRequest;
    case GpuOutputColorDiagnosticCode::WrongThread:
    case GpuOutputColorDiagnosticCode::DeviceUnavailable:
    case GpuOutputColorDiagnosticCode::Busy:
        return GpuProcessFrameDiagnosticCode::DeviceUnavailable;
    case GpuOutputColorDiagnosticCode::DeviceLost:
        return GpuProcessFrameDiagnosticCode::DeviceLost;
    case GpuOutputColorDiagnosticCode::OverBudget:
        return GpuProcessFrameDiagnosticCode::ReadbackOverBudget;
    case GpuOutputColorDiagnosticCode::Cancelled:
        return GpuProcessFrameDiagnosticCode::Cancelled;
    case GpuOutputColorDiagnosticCode::ExecutorRefused:
    case GpuOutputColorDiagnosticCode::ExecutorFailed:
    case GpuOutputColorDiagnosticCode::ReadbackRefused:
    case GpuOutputColorDiagnosticCode::ReadbackFailed:
    case GpuOutputColorDiagnosticCode::InternalInvariant:
        return GpuProcessFrameDiagnosticCode::ReadbackFailed;
    }
    return GpuProcessFrameDiagnosticCode::InternalInvariant;
}

FinalReadbackOutcome runFinalCombinedReadback(
    GpuOutputColorStage& stage, const render::Rgba32fImageDescriptor& descriptor,
    std::shared_ptr<const PreparedGpuOcioCommand> outputCommand,
    std::shared_ptr<const render::GpuImage> image, const std::uint64_t readbackByteBudget,
    const std::chrono::milliseconds nativeDeadline, const CancellationToken& cancellation,
    const std::atomic_bool& stopRequested) {
    FinalReadbackOutcome outcome;
    const std::uint64_t width = descriptor.dataWindow().extent().width();
    const std::uint64_t height = descriptor.dataWindow().extent().height();
    if (width == 0 || height == 0 || width > UINT64_MAX / height ||
        width * height > readbackByteBudget / (2U * sizeof(render::Rgba32f))) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnosticCode = GpuProcessFrameDiagnosticCode::ReadbackOverBudget;
        outcome.diagnosticMessage =
            "the final readback host buffers exceed the readback byte budget";
        return outcome;
    }

    const auto accepted = stage.begin(std::move(outputCommand), std::move(image), readbackByteBudget);
    if (accepted.code != GpuOutputColorDiagnosticCode::None) {
        const auto code = mapOutputColorCode(accepted.code);
        outcome.status = code == GpuProcessFrameDiagnosticCode::Cancelled
                             ? GpuProcessFrameStatus::Cancelled
                             : GpuProcessFrameStatus::Failed;
        outcome.diagnosticCode = code;
        outcome.diagnosticMessage = accepted.message;
        return outcome;
    }
    const auto readbackDeadline = std::chrono::steady_clock::now() + nativeDeadline;
    GpuOutputColorPollResult pollResult = GpuOutputColorPollResult::Pending;
    for (;;) {
        if (cancellation.isCancellationRequested() || stopRequested.load()) {
            stage.cancel();
        }
        pollResult = stage.poll();
        if (pollResult != GpuOutputColorPollResult::Pending) {
            break;
        }
        if (std::chrono::steady_clock::now() >= readbackDeadline) {
            stage.cancel();
            const auto drainDeadline = std::chrono::steady_clock::now() + nativeDeadline;
            while (std::chrono::steady_clock::now() < drainDeadline) {
                pollResult = stage.poll();
                if (pollResult != GpuOutputColorPollResult::Pending) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    if (pollResult != GpuOutputColorPollResult::Ready) {
        const auto code = mapOutputColorCode(stage.diagnostic().code);
        outcome.status = code == GpuProcessFrameDiagnosticCode::Cancelled
                             ? GpuProcessFrameStatus::Cancelled
                             : GpuProcessFrameStatus::Failed;
        outcome.diagnosticCode = code;
        outcome.diagnosticMessage = stage.diagnostic().message;
        return outcome;
    }
    auto colorFrame = stage.take();
    if (!colorFrame.has_value()) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnosticCode = GpuProcessFrameDiagnosticCode::InternalInvariant;
        outcome.diagnosticMessage = "the output-colour stage reported Ready without a frame";
        return outcome;
    }

    auto imageBuilder =
        render::Rgba32fImageBuilder::create(descriptor, static_cast<std::size_t>(readbackByteBudget));
    if (!imageBuilder) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnosticCode = GpuProcessFrameDiagnosticCode::BadAllocation;
        outcome.diagnosticMessage = "the process image host buffer could not be allocated";
        return outcome;
    }
    auto pixels = std::move(colorFrame->process);
    outcome.readbacks = colorFrame->counters.readbackSubmissions;
    auto* imageBuilderPtr = imageBuilder.value();
    const auto originY = descriptor.dataWindow().originY();
    for (std::uint32_t row = 0; row < height; ++row) {
        auto destination = imageBuilderPtr->row(originY + static_cast<std::int64_t>(row));
        if (!destination) {
            outcome.status = GpuProcessFrameStatus::Failed;
            outcome.diagnosticCode = GpuProcessFrameDiagnosticCode::InternalInvariant;
            outcome.diagnosticMessage = "the process image row could not be addressed";
            return outcome;
        }
        const auto sourceOffset = static_cast<std::size_t>(row) * width;
        std::memcpy(destination.value()->data(), pixels.data() + sourceOffset,
                    static_cast<std::size_t>(width) * sizeof(render::Rgba32f));
    }
    pixels.clear();
    pixels.shrink_to_fit();
    auto frozen = std::move(*imageBuilderPtr).freeze();
    if (!frozen) {
        outcome.status = GpuProcessFrameStatus::Failed;
        outcome.diagnosticCode = GpuProcessFrameDiagnosticCode::InternalInvariant;
        outcome.diagnosticMessage = "the process image could not be frozen";
        return outcome;
    }
    outcome.processImage =
        std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));

    outcome.status = GpuProcessFrameStatus::Evaluated;
    outcome.encodedArm = colorFrame->arm;
    outcome.encodedEffectRgba32f = std::move(colorFrame->effectRgba32f);
    outcome.encodedDisplayRgba8 = std::move(colorFrame->displayRgba8);
    outcome.outputCommandIdentity = colorFrame->commandIdentity;
    outcome.outputColorCounters = colorFrame->counters;
    return outcome;
}

} // namespace bloom::runtime::detail
