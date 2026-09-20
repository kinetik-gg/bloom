#pragma once

// Production owner-thread combined readback primitive for the GPU output-colour export boundary.
//
// A final render needs two distinct payloads that cannot be conflated:
//   * the exact RGBA32F process bits the CPU semantic-identity/digest stage hashes, and
//   * the encoded output (straight RGBA8 display bytes, or an output-space RGBA32F effect result).
//
// This primitive copies the process image and (when present) exactly one encoded source into ONE
// bounded host-visible staging allocation with ONE command buffer and ONE fence submission, then
// splits the two regions into their own host vectors. It therefore reports transfer counts
// distinctly and never presents two payloads as a single transfer:
//   * `submissions`  -- always 1 for a completed combined readback,
//   * `payloads`     -- 1 (process only) or 2 (process + encoded),
//   * `bytes`        -- the exact staged byte count, with `processBytes`/`encodedBytes` split.
//
// It is deliberately distinct from the test/oracle-only `readbackResidentImage` /
// `readbackResidentDisplayImage` helpers. Owner-thread only; it follows the production
// `GpuProcessReadback` lifetime model: every source is retained by shared ownership for the whole
// submission, cancellation is a request, and no native resource is destroyed until the fence is
// proven retired (an unproven submission is retained and never published). No
// vkDeviceWaitIdle/vkQueueWaitIdle is ever called.
//
// Source layouts: the process and effect RGBA32F images are resident in GENERAL layout (the
// compute/scene convention); the RGBA8 display image is resident in SHADER_READ_ONLY_OPTIMAL (the
// OCIO display convention). This primitive never mutates process pixel values.

#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::render {

enum class GpuOutputColorReadbackState : std::uint8_t {
    Idle,
    Pending,
    Ready,
    Failure,
};

enum class GpuOutputColorReadbackCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    OverBudget,
    InvalidArgument,
    ReadbackFailed,
    Cancelled,
};

struct GpuOutputColorReadbackDiagnostic final {
    GpuOutputColorReadbackCode code = GpuOutputColorReadbackCode::None;
    std::string message;

    friend bool operator==(const GpuOutputColorReadbackDiagnostic&,
                           const GpuOutputColorReadbackDiagnostic&) = default;
};

// Per-completed-readback transfer accounting. `submissions` is the device-to-host submission count
// (0 or 1 per completed readback), `payloads` is the number of distinct region payloads carried by
// that one submission, and `bytes` is the total staged byte count split into process and encoded.
struct GpuOutputColorReadbackCounters final {
    std::uint64_t submissions = 0;
    std::uint64_t payloads = 0;
    std::uint64_t bytes = 0;
    std::uint64_t processBytes = 0;
    std::uint64_t encodedBytes = 0;

    friend bool operator==(const GpuOutputColorReadbackCounters&,
                           const GpuOutputColorReadbackCounters&) = default;
};

struct GpuOutputColorReadbackPayloads final {
    // Exact process RGBA32F bits, in increasing row Y / increasing X order.
    std::vector<Rgba32f> process;
    // Output-space RGBA32F effect result (FinalRgba32f arm), when staged.
    std::vector<Rgba32f> encodedRgba32f;
    // Straight RGBA8 display result (DisplayRgba8 arm), when staged.
    std::vector<Rgba8> encodedRgba8;
    GpuOutputColorReadbackCounters counters;
};

class GpuOutputColorReadback final {
  public:
    GpuOutputColorReadback();
    GpuOutputColorReadback(const GpuOutputColorReadback&) = delete;
    GpuOutputColorReadback& operator=(const GpuOutputColorReadback&) = delete;
    GpuOutputColorReadback(GpuOutputColorReadback&&) = delete;
    GpuOutputColorReadback& operator=(GpuOutputColorReadback&&) = delete;
    ~GpuOutputColorReadback();

    [[nodiscard]] GpuOutputColorReadbackState state() const noexcept;
    [[nodiscard]] const GpuOutputColorReadbackDiagnostic& diagnostic() const noexcept;
    // True only on the device owner thread of the last submission.
    [[nodiscard]] bool isOwnerThread() const noexcept;

    // Owner-thread. The process image is required. At most one encoded source may be non-null/
    // present; both set is an InvalidArgument. `encodedProcessImage` selects the FinalRgba32f arm;
    // `encodedDisplayImage` (move-only, may be invalid) selects the DisplayRgba8 arm. Validates
    // every source, checks the checked width*height*bytes-per-pixel of each payload, allocates ONE
    // bounded staging buffer, records the region copies, and submits once. The concurrent peak
    // (allocator-rounded staging plus both host vectors) is checked against `byteBudget` before the
    // claim and rechecked against the actual allocation before submission.
    [[nodiscard]] bool begin(std::shared_ptr<const GpuImage> processImage,
                             std::shared_ptr<const GpuImage> encodedProcessImage,
                             std::optional<GpuDisplayImage> encodedDisplayImage,
                             std::uint64_t byteBudget) noexcept;

    // Owner-thread, non-blocking; observes the fence with vkGetFenceStatus. On a proven-signalled
    // fence it copies both staged regions into owned host vectors exactly once and becomes Ready.
    [[nodiscard]] GpuOutputColorReadbackState poll() noexcept;

    // Owner-thread. Only meaningful in Ready; moves the payloads out and resets to Idle.
    [[nodiscard]] GpuOutputColorReadbackPayloads take() noexcept;

    // Owner-thread. Requests cancellation; resources are never freed until the fence is proven.
    void cancel() noexcept;

    // True while a submitted native readback has not proven fence retirement. A caller must retain
    // the stage, its sources, and its budget until this is false.
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::render
