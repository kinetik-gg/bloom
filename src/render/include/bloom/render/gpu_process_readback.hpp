#pragma once

// Production final-output RGBA32F readback primitive.
//
// This is the ONE legitimate asynchronous device->host transfer used by the GPU final-render/export
// bridge. It is deliberately distinct from the synchronous, test/oracle-only
// `readbackResidentImage()` helper (which stays test-only): the export path needs a bounded,
// cancellable, owner-thread lifetime where the source image and every staging/command/fence
// resource are retained until the submission's fence is PROVEN retired, so cancellation, device
// loss, or an unproven timeout never frees a live native resource and never publishes a frame.
//
// Contract:
//  * begin() is owner-thread only, non-blocking, and submits at most one bounded staging transfer.
//  * poll() is owner-thread only and never blocks: it observes the fence with vkGetFenceStatus.
//  * take() transfers the host pixels exactly once and returns to Idle.
//  * cancel() requests cancellation; the source/staging/command/fence are released only after the
//    fence is proven signalled, otherwise they are retained in the bounded process quarantine.
//  * No vkDeviceWaitIdle/vkQueueWaitIdle is ever called.
//  * The preview/display path never uses this class; it exists only at the final CPU output-adapter
//    boundary.

#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace bloom::render {

enum class GpuProcessReadbackState : std::uint8_t {
    Idle,
    Pending,
    Ready,
    Failure,
};

enum class GpuProcessReadbackCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    OverBudget,
    ReadbackFailed,
    Cancelled,
};

struct GpuProcessReadbackDiagnostic final {
    GpuProcessReadbackCode code = GpuProcessReadbackCode::None;
    std::string message;

    friend bool operator==(const GpuProcessReadbackDiagnostic&,
                           const GpuProcessReadbackDiagnostic&) = default;
};

class GpuProcessReadback final {
  public:
    GpuProcessReadback();
    GpuProcessReadback(const GpuProcessReadback&) = delete;
    GpuProcessReadback& operator=(const GpuProcessReadback&) = delete;
    GpuProcessReadback(GpuProcessReadback&&) = delete;
    GpuProcessReadback& operator=(GpuProcessReadback&&) = delete;
    ~GpuProcessReadback();

    [[nodiscard]] GpuProcessReadbackState state() const noexcept;
    [[nodiscard]] const GpuProcessReadbackDiagnostic& diagnostic() const noexcept;

    // Owner-thread. Validates the source and byte budget, allocates one bounded host-visible
    // staging buffer, records the barrier+copy+barrier, submits, and returns true while Pending.
    // The caller-supplied shared_ptr retains the source image (and therefore its device generation)
    // for the whole submission lifetime through the global RESERVED slot, so the readback never
    // keeps a raw source pointer that could outlive the image. The concurrent host-buffer peak this
    // call admits is the actual allocator-rounded staging allocation plus the eventual host pixel
    // vector, both checked against `byteBudget` before submission.
    [[nodiscard]] bool begin(std::shared_ptr<const GpuImage> image,
                             std::uint64_t byteBudget) noexcept;

    // Owner-thread, non-blocking. Returns the current state. On a proven-signalled fence it copies
    // the staging bytes into the owned host pixel vector exactly once and becomes Ready (or Failure
    // for a cancelled request). An unproven past-deadline submission is quarantined.
    [[nodiscard]] GpuProcessReadbackState poll() noexcept;

    // Owner-thread. Only meaningful in Ready; moves the host pixels out and resets to Idle.
    [[nodiscard]] std::vector<Rgba32f> take() noexcept;

    // Owner-thread. Requests cancellation. Resources are never freed until the fence is proven.
    void cancel() noexcept;

    // True on the device-owner thread of the retained source. False for a moved-from/invalid
    // readback or a foreign thread; never touches the driver.
    [[nodiscard]] bool isOwnerThread() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::render
