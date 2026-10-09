#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/gpu_prepared_packed_preview_product.hpp>

#include <algorithm>

namespace bloom::runtime::detail {
namespace {
// Only the unpublished final native source follows a transfer into quarantine. Normal late
// operation separately keeps the stage/admission owner-pumped; bounded shutdown explicitly hands
// physical ownership to the primitive's independent reservation, allowing unrelated context to die.
struct PackedSourcePin final {
    render::GpuDisplayImage image;
};
} // namespace

bool residentPackedTransferBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                 const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                                 std::string& reason) noexcept {
    try {
        if (!core->device || !stage->gpuStage || !stage->gpuStage->scene()) {
            reason = "GPU packed transfer prerequisites are unavailable";
            return false;
        }
        std::optional<render::GpuDisplayImage> image;
        if (stage->gpuStage->hasGeneralDisplayProgram()) {
            if (core->residentGeneralDisplay)
                image = core->residentGeneralDisplay->takeDisplayImage();
        } else if (core->residentDisplay) {
            image = core->residentDisplay->takeImage();
        }
        const auto& descriptor = stage->gpuStage->scene()->outputDescriptor();
        if (!image || !image->isValid() || !image->isBoundTo(*core->device) ||
            image->dataWindow() != descriptor.dataWindow() ||
            image->displayWindow() != descriptor.displayWindow() ||
            image->pixelAspect() != descriptor.pixelAspect()) {
            reason = "final GPU image does not match the trusted prepared scene";
            return false;
        }
        const auto allowance =
            stage->gpuByteAllowance != 0 ? stage->gpuByteAllowance : core->previewByteAllowance();
        const auto sourceBytes = image->allocationBytes();
        const auto metadata = gpuPreparedPackedMetadataByteCost(
            stage->gpuStage->desiredIdentity(), stage->gpuStage->scene()->bounds().capacity());
        if (metadata >= allowance || sourceBytes >= allowance - metadata) {
            reason = "native source leaves no admitted staging/host transfer allowance";
            return false;
        }
        auto pin = std::make_shared<PackedSourcePin>(PackedSourcePin{std::move(*image)});
        // Alias this exact final image without giving any owner an independently escaping image.
        std::shared_ptr<const render::GpuDisplayImage> source(pin, &pin->image);
        if (core->packedRetention && core->packedRetention->snapshot().reservedBytes != 0) {
            reason = "the previous transfer still owns its independent reservation";
            return false;
        }
        core->packedTransfer = std::make_unique<render::GpuDisplayTransfer>();
        if (!core->packedTransfer->begin(*core->device, std::move(source),
                                         std::min(allowance - metadata - sourceBytes,
                                                  core->options.packedTransferByteAllowance),
                                         stage->gpuStage->phaseLease())) {
            reason = core->packedTransfer->diagnostic().message;
            return false;
        }
        {
            std::lock_guard lock(core->stateMutex);
            core->packedRetention = core->packedTransfer->retention();
        }
        core->packedTransferLate = false;
        const auto& charged = core->packedTransfer->accounting();
        core->counterPackedTransfers.fetch_add(charged.submissions, std::memory_order_relaxed);
        core->counterPackedTransferredBytes.fetch_add(charged.logicalBytes,
                                                      std::memory_order_relaxed);
        core->counterFullFrameReadbacks.fetch_add(charged.submissions, std::memory_order_relaxed);
        core->counterPackedPeakBytes.store(
            std::max(core->counterPackedPeakBytes.load(),
                     charged.peakTransferBytes + sourceBytes + metadata),
            std::memory_order_relaxed);
        return true;
    } catch (...) {
        reason = "final GPU packed transfer allocation failed";
        return false;
    }
}

std::optional<PreparedPreviewFrame>
residentPackedTransferFinish(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                             const PreviewDisplayStageRecord& stage, std::string& reason) noexcept {
    if (!core->packedTransfer || !core->device || !stage.gpuStage) {
        reason = "final GPU packed transfer is unavailable";
        return std::nullopt;
    }
    auto result = core->packedTransfer->take();
    if (!result) {
        reason = "final GPU packed transfer has no fence-proven output";
        return std::nullopt;
    }
    auto frame = makeGpuPreparedPackedPreview(*core->device, core->generation, *stage.gpuStage,
                                              core->residentQualification, std::move(*result),
                                              stage.pixelStorageByteLimit);
    if (!frame) {
        reason = "final GPU packed product identity/geometry/host admission was refused";
        return std::nullopt;
    }
    core->counterDisplayStatusReads.fetch_add(1, std::memory_order_relaxed);
    core->counterResidentCompletions.fetch_add(1, std::memory_order_relaxed);
    return frame;
}
} // namespace bloom::runtime::detail
