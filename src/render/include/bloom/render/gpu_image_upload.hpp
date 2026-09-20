#pragma once

// Bloom-owned, Qt-free and Vulkan-free upload of one already-decoded host RGBA32F image into a
// resident GpuImage.
//
// This is the missing entry point between the CPU media workers and the resident compositing
// operations (SolidV1 / TranslationOpacityBilinearV1 / SourceOverV1 / resident display): the CPU
// worker decodes and colour-converts a still or video frame exactly as it does today, then hands
// the resulting render::Rgba32fImage here once. The bytes are copied into an owned staging buffer
// and a vkCmdCopyBufferToImage writes a device-local RGBA32F storage image; no compute shader is
// involved and nothing is decoded or evaluated on the GPU thread.
//
// The input descriptor (data window, display window, pixel aspect) is preserved verbatim on the
// resident image, so downstream transform/bounds math sees the same geometry the CPU evaluator
// used. The normal result is resident; full readback exists only for CPU-oracle parity tests.
//
// One job per pipeline. begin/poll/image/takeImage/readback/cancel and destruction are
// owner-thread-only and fail closed from another thread. A submission's staging buffer, command
// buffer, and fence are never destroyed while in flight: cancellation marks a discard, and an
// unknown fence result retains the whole job until a known retirement or the bounded resident-pool
// owner drain proves it.
//
// Media decode/source validation stays on the CPU worker and is deliberately not part of this
// API; this op never touches the filesystem.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bloom::render {

class Rgba32fImage;

enum class GpuImageUploadJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuImageUploadPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuImageUploadDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    Unsupported,
    OverBudget,
    Busy,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    AllocationFailed,
    Cancelled,
    NativeTimeout,
};

struct GpuImageUploadDiagnostic final {
    GpuImageUploadDiagnosticCode code = GpuImageUploadDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuImageUploadDiagnostic&,
                           const GpuImageUploadDiagnostic&) = default;
};

// The host image to upload. It is retained as shared immutable ownership for the duration of
// begin(); begin() copies the pixels into an owned staging buffer before returning, so no borrowed
// pointer survives the call and the caller may release its reference immediately afterwards.
struct GpuImageUploadParameters final {
    std::shared_ptr<const Rgba32fImage> source;
};

// Separately bounded image and staging ceilings. The per-call byteBudget is the peak allowance and
// must cover allocator rounding of both allocations, because the authoritative check re-reads the
// actual VMA allocation sizes after creation.
struct GpuImageUploadBudgets final {
    std::uint64_t maxImageBytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maxStagingBytes = 256ULL * 1024ULL * 1024ULL;
};

struct GpuImageUploadCreateResult;

class GpuImageUpload final {
  public:
    GpuImageUpload(const GpuImageUpload&) = delete;
    GpuImageUpload& operator=(const GpuImageUpload&) = delete;
    GpuImageUpload(GpuImageUpload&& other) noexcept;
    GpuImageUpload& operator=(GpuImageUpload&& other) noexcept;
    ~GpuImageUpload();

    // Prepares the upload on the device owner thread. Creation is lazy: an idle instance allocates
    // no native resources, and the command pool/command buffer/fence are allocated on the first
    // begin under a bounded process-wide resident slot. No shader, no pipeline. Wrong thread returns
    // WrongThread.
    [[nodiscard]] static GpuImageUploadCreateResult
    create(GpuDevice& device, const GpuImageUploadBudgets& budgets = {});

    [[nodiscard]] GpuImageUploadJobState state() const noexcept;
    [[nodiscard]] const GpuImageUploadDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    // True while a submitted copy's fence has not been PROVEN retired. No allocation or Vulkan
    // call; a Failure poll with this true is unproven and must be drained.
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    // The actual VMA bytes the most recent accepted job retained during the job (resident image
    // plus its transient staging buffer), or 0 before the first job. Real allocator sizes, never a
    // requested extent.
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates the descriptor/extent/budget/device limits, copies the host pixels into an owned
    // staging buffer, creates the resident image, records one buffer-to-image copy, and submits.
    // None when accepted; otherwise InvalidArgument/Busy/WrongThread/Unsupported/OverBudget/
    // AllocationFailed/DeviceLost.
    [[nodiscard]] GpuImageUploadDiagnostic begin(const GpuImageUploadParameters& parameters,
                                                 std::uint64_t byteBudget);

    // Non-blocking fence query. Pending/Ready/Failure; WrongThread from a foreign thread.
    [[nodiscard]] GpuImageUploadPollResult poll();

    // The resident image while Ready; nullptr otherwise. Ownership stays with the pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident image out and returns the pipeline to Idle.
    [[nodiscard]] GpuImage takeImage() noexcept;

    // Test/oracle-only readback of the resident image. Never required by the normal path.
    [[nodiscard]] GpuImageReadback readback() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence retires.
    void cancel() noexcept;

    // Recoverable bounded-pool pressure, not a permanent fuse: true while a foreign-released or
    // unproven resident is retained for owner drain, and false again once the rightful owner thread
    // has proved retirement (or device loss) and freed it.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuImageUpload(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuImageUploadCreateResult final {
    std::unique_ptr<GpuImageUpload> upload;
    GpuImageUploadDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return upload != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
