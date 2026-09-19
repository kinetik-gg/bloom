#pragma once

// Bloom-owned, Qt-free and Vulkan-free SolidV1 compute operation.
//
// Writes one validated premultiplied lin_rec709_scene RGBA32F pixel into a
// GPU-resident image and retains that image in device memory. The normal result
// is resident: no CPU readback happens on begin/poll/image(). readback() exists
// only for parity tests and the CPU oracle.
//
// One pipeline per device, one outstanding job, owner-thread
// begin/poll/image/readback/cancel and destruction (matching the other render
// GPU operations). Wrong-thread calls fail closed without mutating owned state.
// A submission's command buffer, image, and fence are never destroyed while in
// flight: cancellation marks a discard and the caller must destroy or drain the
// pipeline on the owner thread before reuse.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {

enum class GpuSolidJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuSolidPollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuSolidDiagnosticCode : std::uint8_t {
    None,
    InvalidArgument,
    Unsupported,
    OverBudget,
    Busy,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    ShaderRejected,
    AllocationFailed,
    Cancelled,
    NativeTimeout,
};

struct GpuSolidDiagnostic final {
    GpuSolidDiagnosticCode code = GpuSolidDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuSolidDiagnostic&, const GpuSolidDiagnostic&) = default;
};

// The validated premultiplied pixel plus the destination window/geometry. The
// caller obtains the pixel from the CPU solid primitive
// (solidPixelFromStraightLinearRec709Scene) so CPU and GPU agree.
struct GpuSolidParameters final {
    Rgba32f pixel = Rgba32f::transparent();
    ImageWindow dataWindow;
    ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
};

// Hard ceiling on the retained resident image bytes (width*height*16). The
// per-call byteBudget is a tighter allowance; the actual required bytes must
// fit both.
struct GpuSolidBudgets final {
    std::uint64_t maxImageBytes = 256ULL * 1024ULL * 1024ULL;
};

struct GpuSolidCreateResult;

class GpuSolid final {
  public:
    GpuSolid(const GpuSolid&) = delete;
    GpuSolid& operator=(const GpuSolid&) = delete;
    GpuSolid(GpuSolid&& other) noexcept;
    GpuSolid& operator=(GpuSolid&& other) noexcept;
    ~GpuSolid();

    // Builds the cached shader module, descriptor layout, pipeline layout,
    // compute pipeline, command pool, and fence once on the device owner thread.
    // Wrong thread returns WrongThread.
    [[nodiscard]] static GpuSolidCreateResult create(GpuDevice& device,
                                                     const GpuSolidBudgets& budgets = {});

    [[nodiscard]] GpuSolidJobState state() const noexcept;
    [[nodiscard]] const GpuSolidDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;

    // Validates geometry/budget/device limits, creates the resident image,
    // records and submits one dispatch. Returns None when accepted; otherwise
    // Busy/WrongThread/Unsupported/OverBudget.
    [[nodiscard]] GpuSolidDiagnostic begin(const GpuSolidParameters& parameters,
                                           std::uint64_t byteBudget);

    // Non-blocking fence query. Pending/Ready/Failure; WrongThread from a foreign
    // thread.
    [[nodiscard]] GpuSolidPollResult poll();

    // The resident image while Ready; nullptr otherwise. Ownership stays with the
    // pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident image out and returns the pipeline to Idle. After this,
    // image() is null.
    [[nodiscard]] GpuImage takeImage() noexcept;

    // Test/oracle-only readback of the resident image. Never required by the
    // normal path.
    [[nodiscard]] GpuImageReadback readback() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence
    // retires.
    void cancel() noexcept;

    // Process-global reported limitation: teardown could not drain an in-flight
    // job within its bounded budget and retained (did not destroy) busy Vulkan
    // objects.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuSolid(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuSolidCreateResult final {
    std::unique_ptr<GpuSolid> solid;
    GpuSolidDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return solid != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
