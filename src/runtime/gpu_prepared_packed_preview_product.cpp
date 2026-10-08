#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_prepared_packed_preview_product.hpp>

#include <limits>

namespace bloom::runtime {
std::size_t gpuPreparedPackedMetadataByteCost(const PreviewRequestIdentity& identity,
                                              const std::size_t boundsCapacity) noexcept {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    constexpr std::size_t fixedBytes =
        sizeof(PreviewDisplayOnlyFrame) + sizeof(PreparedPreviewFrame) +
        sizeof(GpuPreparedPackedProvenance) + 4 * sizeof(std::shared_ptr<const void>);
    if (boundsCapacity > (max - fixedBytes) / sizeof(EvaluatedOperationBounds))
        return max;
    auto bytes = fixedBytes + boundsCapacity * sizeof(EvaluatedOperationBounds);
    for (const auto capacity : {identity.displayName.capacity(), identity.viewName.capacity(),
                                identity.displayName.capacity(), identity.viewName.capacity()}) {
        if (capacity > max - bytes)
            return max;
        bytes += capacity;
    }
    return bytes;
}

std::optional<PreparedPreviewFrame> makeGpuPreparedPackedPreview(
    render::GpuDevice& device, const GpuServiceGeneration serviceGeneration,
    const PreviewGpuSceneStage& stage,
    std::shared_ptr<const GpuResidentPreviewQualificationReport> neutralQualification,
    render::GpuDisplayTransferResult&& transfer, const std::size_t byteLimit) noexcept {
    try {
        if (!device.isOwnerThread() || !serviceGeneration.isValid() ||
            !transfer.isBoundTo(device) || stage.scene() == nullptr ||
            stage.displayProcessor() == nullptr)
            return std::nullopt;
        const auto& identity = stage.desiredIdentity();
        auto process = stage.scene()->processIdentity();
        const auto& plan = process.plan;
        const auto& expected = stage.scene()->outputDescriptor();
        if (!plan || identity.requestGeneration == 0 ||
            identity.output != PreviewOutput::Composition ||
            identity.projectId != plan->projectId() ||
            identity.compositionId != plan->compositionId() ||
            identity.sourceRevision != plan->sourceRevision() || process.output != plan->output() ||
            identity.time != process.time || identity.resolution != process.resolution ||
            identity.quality != process.quality || identity.colorIntent != process.colorIntent ||
            identity.roi != process.roi || identity.showLook == process.bypassLookNodes ||
            transfer.dataWindow() != expected.dataWindow() ||
            transfer.displayWindow() != expected.displayWindow() ||
            transfer.pixelAspect() != expected.pixelAspect())
            return std::nullopt;
        core::Sha256Digest command{};
        if (stage.hasGeneralDisplayProgram()) {
            const auto& program = *stage.displayProgram();
            if (!program.command || program.viewAdjust != identity.viewAdjust ||
                program.command->viewAdjust() != identity.viewAdjust ||
                program.width != expected.dataWindow().extent().width() ||
                program.height != expected.dataWindow().extent().height() ||
                program.command->geometry().width != program.width ||
                program.command->geometry().height != program.height ||
                !gpuOcioDisplayCommandIsDisplay(*program.command) ||
                !gpuDisplayProgramMatchesRequest(
                    program, gpuDisplayColorBindingForIntent(
                                 identity.colorIntent, identity.displayName, identity.viewName)))
                return std::nullopt;
            command = program.command->identity();
            neutralQualification.reset();
        } else if (!neutralQualification ||
                   !neutralQualification->eligibleFor(device, *stage.displayProcessor()) ||
                   !(identity.viewAdjust == ViewAdjust{})) {
            return std::nullopt;
        }
        const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
            expected.dataWindow(), expected.pixelAspect());
        if (!descriptor || transfer.pixels().size() != descriptor.value()->layout().pixelCount ||
            transfer.accounting().submissions != 1 ||
            transfer.accounting().hostStorageBytes < transfer.pixels().size_bytes())
            return std::nullopt;
        // Allocate metadata before moving pixels. Charge actual vector capacities, identity
        // storage and fixed proof/frame metadata; final packed retention never charges a native
        // allocation it no longer owns.
        std::vector<EvaluatedOperationBounds> bounds(stage.scene()->bounds());
        const auto metadata = gpuPreparedPackedMetadataByteCost(identity, bounds.capacity());
        if (metadata > byteLimit || transfer.accounting().hostStorageBytes > byteLimit - metadata)
            return std::nullopt;
        auto proof = std::make_shared<const GpuPreparedPackedProvenance>(
            GpuPreparedPackedProvenance{serviceGeneration.value(), transfer.deviceGeneration(),
                                        command, transfer.dataWindow(), transfer.displayWindow(),
                                        transfer.accounting(), std::move(neutralQualification)});
        auto pixels = std::move(transfer).takePixels();
        auto buffer = render::PreparedReferenceDisplayBuffer::adopt(
            *descriptor.value(), std::move(pixels), byteLimit - metadata);
        if (!buffer)
            return std::nullopt;
        process.provider = EvaluationProvider::GpuResident;
        PreviewDisplayOnlyFrame display(identity, std::move(process), std::move(*buffer.value()),
                                        {.provider = PreviewDisplayProvider::GpuPreparedPacked,
                                         .gpuQualification = nullptr,
                                         .gpuPreparedPacked = std::move(proof)},
                                        std::move(bounds));
        return PreparedPreviewFrame::createDisplayOnly(
            identity.requestGeneration,
            std::make_shared<const PreviewDisplayOnlyFrame>(std::move(display)));
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace bloom::runtime
