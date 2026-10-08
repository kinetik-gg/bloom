#pragma once

#include <bloom/render/gpu_display_transfer.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>

namespace bloom::runtime {

// Immutable evidence for a host-only display product: actual scene preparation stays GPU, while
// the final packed transfer permits ordinary widget composition. No native presentation claim.
struct GpuPreparedPackedProvenance final {
    std::uint64_t serviceGeneration = 0;
    std::uint32_t deviceGeneration = 0;
    core::Sha256Digest displayCommandIdentity{};
    std::optional<render::ImageWindow> dataWindow, displayWindow;
    render::GpuDisplayTransferAccounting transfer;
    std::shared_ptr<const GpuResidentPreviewQualificationReport> neutralQualification;
};

[[nodiscard]] std::size_t gpuPreparedPackedMetadataByteCost(const PreviewRequestIdentity& identity,
                                                            std::size_t boundsCapacity) noexcept;

// Owner-thread finalizer. Only a fence-proven sealed transfer from this exact device, matching the
// trusted prepared scene and its selected display program, can construct GPU-prepared packed
// provenance. The result retains host storage/identity only; temporary device bytes retire first.
[[nodiscard]] std::optional<PreparedPreviewFrame> makeGpuPreparedPackedPreview(
    render::GpuDevice& device, GpuServiceGeneration serviceGeneration,
    const PreviewGpuSceneStage& stage,
    std::shared_ptr<const GpuResidentPreviewQualificationReport> neutralQualification,
    render::GpuDisplayTransferResult&& transfer, std::size_t byteLimit) noexcept;

} // namespace bloom::runtime
