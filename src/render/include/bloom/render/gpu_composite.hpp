#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU compositing operations: TranslationOpacityBilinearV1
// and SourceOverV1. Each writes a GPU-resident RGBA32F GpuImage; the normal path never reads back.
//
// This header is intentionally narrow. It does not create a device or a service; it consumes the
// existing GpuDevice and the resident GpuImage from gpu_image.hpp. Native work runs on the device
// owner thread and fails closed from any other thread. One job is outstanding at a time.
//
// The CPU primitives in cpu_image_primitives.hpp remain the correctness oracle. Both operations
// are compared to them under the documented per-finite-component 2e-6 absolute-or-relative gate
// (docs/architecture/gpu-backend.md), never claimed bit-exact.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {

// --- Host-prepared axis samples (TranslationOpacityBilinearV1) --------------------------------
//
// The CPU kernel computes the sample point in Float64 and the interpolation factor as a rounded
// Float32. A GPU kernel cannot do that subtraction in Float32 without losing precision at large
// output coordinates, so the host prepares, for each output column and row, the exact CPU
// floor/factor once. This is O(width + height) small metadata work, off the UI thread, with no
// per-pixel CPU computation and no shaderFloat64.
struct GpuAxisSample final {
    // floor(sampleLocal), or kGpuAxisOutOfRange when the CPU rule would make this row/column
    // transparent (sampleLocal <= -1.0 or >= sourceExtent).
    std::int32_t base = 0;
    float factor = 0.0F;
};

// The sentinel base meaning "this axis sample is out of range; emit transparent".
inline constexpr std::int32_t kGpuAxisOutOfRange = std::numeric_limits<std::int32_t>::min();

// Exact CPU axis preparation: sampleLocal = (double)local - translation; sentinel when
// sampleLocal <= -1.0 or sampleLocal >= sourceExtent; otherwise base = (int64)floor(sampleLocal)
// and factor = (float)(sampleLocal - (double)base). Reproduces
// translateOpacityBilinearRow()'s per-axis arithmetic exactly.
[[nodiscard]] std::vector<GpuAxisSample>
prepareTranslationAxis(std::uint32_t outputExtent, std::uint32_t sourceExtent, double translation);

// --- Operations --------------------------------------------------------------------------------

enum class GpuCompositeJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuCompositePollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuCompositeDiagnosticCode : std::uint8_t {
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
    // The kernel's status/error flag was nonzero: the frame must not be published and the caller
    // falls back to the CPU reference path.
    StatusFlagRejected,
};

struct GpuCompositeDiagnostic final {
    GpuCompositeDiagnosticCode code = GpuCompositeDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuCompositeDiagnostic&, const GpuCompositeDiagnostic&) = default;
};

// Validated inputs for TranslationOpacityBilinearV1. The source is shared immutable ownership
// (retained for the job's lifetime) so it can come straight from the content cache. `outputWindow`
// is the output DATA window only; the output DISPLAY window and pixel aspect are preserved from the
// source so a nonzero-origin composition keeps its geometry. Translation is Float64 and opacity is
// already rounded once to Float32 by the CPU primitive.
struct GpuTranslationParameters final {
    std::shared_ptr<const GpuImage> source;
    ImageWindow outputWindow;
    double translationX = 0.0;
    double translationY = 0.0;
    float opacity = 1.0F;
};

// Validated inputs for SourceOverV1. `source` is the foreground, `destination` the backdrop; both
// are shared immutable ownership retained for the job's lifetime. Neither input is mutated: the
// result is a new resident output image.
struct GpuSourceOverParameters final {
    std::shared_ptr<const GpuImage> source;
    std::shared_ptr<const GpuImage> destination;
};

struct GpuCompositeBudgets final {
    std::uint64_t maxImageBytes = 256ULL * 1024ULL * 1024ULL;
    // Caps the copied axis/status buffers (translation only) in addition to the image budget.
    std::uint64_t maxMetadataBytes = 16ULL * 1024ULL * 1024ULL;
};

struct GpuCompositeCreateResult;

class GpuComposite final {
  public:
    GpuComposite(const GpuComposite&) = delete;
    GpuComposite& operator=(const GpuComposite&) = delete;
    GpuComposite(GpuComposite&& other) noexcept;
    GpuComposite& operator=(GpuComposite&& other) noexcept;
    ~GpuComposite();

    // Builds the cached pipelines, layouts, descriptor sets, command pool, and fence for BOTH
    // operations once, on the device owner thread. Wrong thread returns WrongThread.
    [[nodiscard]] static GpuCompositeCreateResult create(GpuDevice& device,
                                                         const GpuCompositeBudgets& budgets = {});

    [[nodiscard]] GpuCompositeJobState state() const noexcept;
    [[nodiscard]] const GpuCompositeDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Both validate geometry/budgets/device limits, prepare host metadata, create the resident
    // image, and submit one dispatch. None when accepted.
    [[nodiscard]] GpuCompositeDiagnostic
    beginTranslation(const GpuTranslationParameters& parameters, std::uint64_t byteBudget);
    [[nodiscard]] GpuCompositeDiagnostic beginSourceOver(const GpuSourceOverParameters& parameters,
                                                         std::uint64_t byteBudget);

    // Non-blocking fence query. On Ready the status flag word has been checked; a nonzero flag
    // publishes Failure(StatusFlagRejected) instead of Ready.
    [[nodiscard]] GpuCompositePollResult poll();

    // The resident output image while Ready; nullptr otherwise. Ownership stays with the pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident output image out and returns the pipeline to Idle. Only valid when Ready.
    [[nodiscard]] GpuImage takeImage() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence retires.
    void cancel() noexcept;

    // Process-global reported limitation: teardown could not drain an in-flight job within its
    // bounded budget and retained (did not destroy) busy Vulkan objects.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuComposite(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuCompositeCreateResult final {
    std::unique_ptr<GpuComposite> composite;
    GpuCompositeDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return composite != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
