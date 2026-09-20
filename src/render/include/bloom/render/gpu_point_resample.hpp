#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU point/nearest-neighbour image resampler:
// PointResampleV1. Additive to gpu_composite.hpp/gpu_affine.hpp; it changes neither the affine
// bilinear operation nor the composite/blend/solid operations, and it does not touch the runtime
// media-image proxy CPU oracle.
//
// The operation reads ONE resident, device-bound source GpuImage and publishes a new resident
// RGBA32F GpuImage whose pixels are the source pixels selected by the media-image proxy CPU
// mapping in src/runtime/image_source.cpp evaluateImageSource(). It is a BIT-EXACT COPY: no
// interpolation, no colour transform, no alpha change, no readback, and no host pixel sampling.
//
// CPU oracle (must be reproduced exactly), for a decoded image of data-window extent
// sourceWidth x sourceHeight, positive finite horizontalScale/verticalScale, and a composition
// display window/pixel aspect:
//   outputWidth  = max(1, ceil(sourceWidth  * horizontalScale))
//   outputHeight = max(1, ceil(sourceHeight * verticalScale))
//   sourceX(x)   = min(sourceWidth  - 1, (uint32)(double(x) / horizontalScale))
//   sourceY(y)   = min(sourceHeight - 1, (uint32)(double(y) / verticalScale))
//   out[y][x]    = in[sourceY(y)][sourceX(x)]
// Source data-window origin is irrelevant to indexing (indices are data-window-relative); output
// origin is (0, 0); the display window and pixel aspect are preserved verbatim.
//
// SAMPLE METADATA. The exact binary64 `x / scale` axis mapping is evaluated once per output column
// and once per output row on the host and uploaded as a bounded O(width + height) int32 storage
// buffer (sourceX first, then sourceY). The kernel only fetches and stores that pixel; it never
// re-derives the mapping and needs no shaderFloat64. No O(width*height) host image sampling or
// per-pixel precompute happens; the RGBA pixels are only ever read on the GPU.
//
// This header is intentionally narrow. It does not create a device or a service; it consumes the
// existing GpuDevice and GpuImage. Native work runs on the device owner thread and fails closed
// from any other thread. One job is outstanding at a time.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {

// Exact host axis maps for one resample. `sourceX` has outputWidth entries and `sourceY` has
// outputHeight entries; each value is the data-window-relative source index the CPU oracle selects.
struct GpuPointResampleAxisMaps final {
    std::vector<std::int32_t> sourceX;
    std::vector<std::int32_t> sourceY;

    friend bool operator==(const GpuPointResampleAxisMaps&,
                           const GpuPointResampleAxisMaps&) noexcept = default;
};

// Exact host preparation of the two independent axis maps using the CPU oracle's binary64
// expression. Pure host arithmetic; identical in the Vulkan and stub builds.
[[nodiscard]] GpuPointResampleAxisMaps
preparePointResampleAxisMaps(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                             std::uint32_t outputWidth, std::uint32_t outputHeight,
                             double horizontalScale, double verticalScale);

// Immutable inputs for one resample. `source` is shared immutable ownership retained for the job's
// lifetime. `output` is the validated output descriptor: its data window must be exactly the proxy
// window `(0, 0, max(1, ceil(sourceWidth*scale)), max(1, ceil(sourceHeight*scale)))` and its
// display window/pixel aspect are preserved verbatim on the published image.
struct GpuPointResampleRequest final {
    std::shared_ptr<const GpuImage> source;
    Rgba32fImageDescriptor output;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
};

// Per-call ceilings. Defaults are effectively unbounded so the real bounds are the live device
// limits and the per-call byteBudget; no fixed 256 MiB or 4K ceiling is baked in. A non-zero value
// can be injected to prove the fail-closed budget path.
struct GpuPointResampleBudgets final {
    std::uint64_t maxImageBytes = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxMetadataBytes = std::numeric_limits<std::uint64_t>::max();
};

enum class GpuPointResampleJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuPointResamplePollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuPointResampleDiagnosticCode : std::uint8_t {
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

struct GpuPointResampleDiagnostic final {
    GpuPointResampleDiagnosticCode code = GpuPointResampleDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuPointResampleDiagnostic&,
                           const GpuPointResampleDiagnostic&) = default;
};

struct GpuPointResampleCreateResult;

class GpuPointResample final {
  public:
    GpuPointResample(const GpuPointResample&) = delete;
    GpuPointResample& operator=(const GpuPointResample&) = delete;
    GpuPointResample(GpuPointResample&& other) noexcept;
    GpuPointResample& operator=(GpuPointResample&& other) noexcept;
    ~GpuPointResample();

    // Builds the cached PointResampleV1 pipeline, layout, descriptor set, command pool, and fence
    // once, on the device owner thread. Wrong thread returns WrongThread.
    [[nodiscard]] static GpuPointResampleCreateResult
    create(GpuDevice& device, const GpuPointResampleBudgets& budgets = {});

    [[nodiscard]] GpuPointResampleJobState state() const noexcept;
    [[nodiscard]] const GpuPointResampleDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates geometry/scales/device limits/budget, prepares the host axis maps, creates the
    // resident output image, and submits one dispatch. None when accepted.
    [[nodiscard]] GpuPointResampleDiagnostic begin(const GpuPointResampleRequest& parameters,
                                                   std::uint64_t byteBudget);

    // Non-blocking fence query. On Ready the resident output is published.
    [[nodiscard]] GpuPointResamplePollResult poll();

    // The resident output image while Ready; nullptr otherwise. Ownership stays with the pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident output image out and returns the pipeline to Idle. Only valid when Ready.
    [[nodiscard]] GpuImage take() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence retires.
    void cancel() noexcept;

    // Process-global reported limitation: teardown could not drain an in-flight resample job within
    // its bounded budget and retained (did not destroy) busy Vulkan objects.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuPointResample(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuPointResampleCreateResult final {
    std::unique_ptr<GpuPointResample> resampler;
    GpuPointResampleDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return resampler != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
