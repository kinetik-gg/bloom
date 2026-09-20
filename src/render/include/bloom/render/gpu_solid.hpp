#pragma once

// Bloom-owned, Qt-free and Vulkan-free SolidV1 / CoveredSolidV1 compute operations.
//
// SolidV1 (ordinary, unmasked): writes one validated premultiplied lin_rec709_scene
// RGBA32F pixel into a GPU-resident image and retains that image in device memory.
// The normal result is resident: no CPU readback happens on begin/poll/image().
//
// CoveredSolidV1 (beginCovered): the CPU path a fractional Solid transform takes. The
// host supplies the exact immutable R8 coverage bitmap the CPU PathRaster produces plus
// the layer's separate Float32 opacity, and this operation materialises the same
// premultiplied RGBA32F resident image the CPU vector arm does. The host precomputes a
// 256-entry palette with the EXISTING render::coverageSolidRow() primitive (one entry
// per coverage byte) followed by the exact separate Float32 opacity multiply and Rgba32f
// validation; the shader only selects a stored palette entry, so the resident result is
// bit-exact to the CPU arm. It is O(256) host metadata, never a full-frame CPU render,
// and no Float64 arithmetic is required on the GPU. Bilinear translation of a filled
// bitmap is deliberately NOT used: that would produce different pixels.
//
// readback()/readbackResidentImage() exist only for parity tests and the CPU oracle.
//
// One pipeline set per device, one outstanding job, owner-thread
// begin/beginCovered/poll/image/readback/cancel and destruction (matching the other
// render GPU operations). Wrong-thread calls fail closed without mutating owned state.
// A submission's command buffer, image, mask/palette buffers, and fence are never
// destroyed while in flight: cancellation marks a discard and the caller must destroy or
// drain the pipeline on the owner thread before reuse.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bloom::render {

class GpuPathCoverage;

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

    // Prepares the SolidV1 operation on the device owner thread. Creation is lazy: an idle instance
    // allocates no native resources, and the cached shader module, descriptor layout, pipeline
    // layout, compute pipeline, command pool, and fence are built on the first begin under a bounded
    // process-wide resident slot. Wrong thread returns WrongThread.
    [[nodiscard]] static GpuSolidCreateResult create(GpuDevice& device,
                                                     const GpuSolidBudgets& budgets = {});

    [[nodiscard]] GpuSolidJobState state() const noexcept;
    [[nodiscard]] const GpuSolidDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates geometry/budget/device limits, creates the resident image,
    // records and submits one dispatch. Returns None when accepted; otherwise
    // Busy/WrongThread/Unsupported/OverBudget.
    [[nodiscard]] GpuSolidDiagnostic begin(const GpuSolidParameters& parameters,
                                           std::uint64_t byteBudget);

    // CoveredSolidV1. `base.pixel` is the resolved premultiplied solid pixel and
    // `base.dataWindow`/`base.displayWindow`/`base.pixelAspect` are the coverage
    // window and the untouched display/PAR metadata. `coverage` is exactly
    // width*height contiguous R8 bytes (row-major over the data window) matching
    // what CPU PathRaster::coverageRow produced; it is copied before this call
    // returns, so the caller may release it immediately. `opacity` is the layer's
    // separate Float32 opacity and must be finite and within [0, 1]. The resident
    // image is bit-exact to the CPU coverageSolidRow + separate-opacity arm.
    // Rejections: WrongThread/Busy/DeviceLost/InvalidArgument (size, opacity,
    // non-finite palette)/Unsupported/OverBudget. Ownership and cancellation
    // behave exactly like begin().
    [[nodiscard]] GpuSolidDiagnostic beginCovered(const GpuSolidParameters& base,
                                                  std::span<const std::uint8_t> coverage,
                                                  float opacity, std::uint64_t byteBudget);

    // ResidentCoveredSolidV1. Consumes the mask produced by a Ready
    // GpuPathCoverage directly, binding its device-resident packed R8 buffer with
    // no download or re-upload and co-owning it until this submission retires.
    // `base.pixel`/windows/PAR and `opacity` have the same meaning as
    // beginCovered; `coverage.coverageWidth()/coverageHeight()` must equal the
    // data-window extent. Rejections mirror beginCovered plus InvalidArgument for
    // a non-Ready or mismatched coverage.
    [[nodiscard]] GpuSolidDiagnostic beginCoveredResident(const GpuSolidParameters& base,
                                                          const GpuPathCoverage& coverage,
                                                          float opacity,
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

    // Recoverable bounded-pool pressure, not a permanent fuse: true while a foreign-released or
    // unproven resident is retained for owner drain, and false again once the rightful owner thread
    // has proved retirement (or device loss) and freed it.
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
