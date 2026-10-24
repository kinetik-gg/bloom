#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU Cryptomatte coverage: CryptomatteCoverageV1 (#364).
//
// The operation reads ONE resident, device-bound RGBA32F source holding the Cryptomatte rank
// pairs of a layer stacked vertically (rank r occupies rows [r * height, (r + 1) * height)): R is
// the ID's exact float bits and G its coverage (B = 0, A = 1). It publishes a new resident
// RGBA32F image of `width` x `height` whose every lane is the matte coverage:
//   c = 0
//   for r in 0 .. rankCount-1:          (rank order, exactly as the CPU reference)
//       id = floatBitsToUint(source[x][y + r * height].r)
//       c  = clamp(c + (id in ids ? source[x][y + r * height].g : 0), 0, 1)
//   out[x][y] = (c, c, c, c)              (premultiplied white x coverage, the ADR 0028 Mask)
// The selected IDs are uploaded once as a bounded uint32 storage buffer. The kernel performs only
// bit comparisons, one Float32 add per matching rank and a clamp, so it is bit-identical to the
// CPU reference in src/runtime/cryptomatte.cpp.
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
#include <optional>
#include <string>
#include <vector>

namespace bloom::render {

// Immutable inputs for one coverage pass. `source` is the stacked rank image (width x height *
// rankCount data window); `output` is the published descriptor whose data window has the source's
// width, height and origin, and whose display window/pixel aspect are preserved verbatim.
struct GpuCryptomatteCoverageRequest final {
    std::shared_ptr<const GpuImage> source;
    Rgba32fImageDescriptor output;
    std::uint32_t rankCount = 0;
    std::vector<std::uint32_t> ids;
};

// Per-call ceilings. Defaults are effectively unbounded so the real bounds are the live device
// limits and the per-call byteBudget; no fixed 256 MiB or 4K ceiling is baked in. A non-zero value
// can be injected to prove the fail-closed budget path.
struct GpuCryptomatteCoverageBudgets final {
    std::uint64_t maxImageBytes = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxMetadataBytes = std::numeric_limits<std::uint64_t>::max();
};

enum class GpuCryptomatteCoverageJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuCryptomatteCoveragePollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuCryptomatteCoverageDiagnosticCode : std::uint8_t {
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

struct GpuCryptomatteCoverageDiagnostic final {
    GpuCryptomatteCoverageDiagnosticCode code = GpuCryptomatteCoverageDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuCryptomatteCoverageDiagnostic&,
                           const GpuCryptomatteCoverageDiagnostic&) = default;
};

struct GpuCryptomatteCoverageCreateResult;

class GpuCryptomatteCoverage final {
  public:
    GpuCryptomatteCoverage(const GpuCryptomatteCoverage&) = delete;
    GpuCryptomatteCoverage& operator=(const GpuCryptomatteCoverage&) = delete;
    GpuCryptomatteCoverage(GpuCryptomatteCoverage&& other) noexcept;
    GpuCryptomatteCoverage& operator=(GpuCryptomatteCoverage&& other) noexcept;
    ~GpuCryptomatteCoverage();

    // Builds the cached CryptomatteCoverageV1 pipeline, layout, descriptor set, command pool, and fence
    // once, on the device owner thread. Wrong thread returns WrongThread.
    [[nodiscard]] static GpuCryptomatteCoverageCreateResult
    create(GpuDevice& device, const GpuCryptomatteCoverageBudgets& budgets = {});

    [[nodiscard]] GpuCryptomatteCoverageJobState state() const noexcept;
    [[nodiscard]] const GpuCryptomatteCoverageDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates geometry/rank count/device limits/budget, uploads the selected IDs, creates the
    // resident output image, and submits one dispatch. None when accepted.
    [[nodiscard]] GpuCryptomatteCoverageDiagnostic begin(const GpuCryptomatteCoverageRequest& parameters,
                                                   std::uint64_t byteBudget);

    // Non-blocking fence query. On Ready the resident output is published.
    [[nodiscard]] GpuCryptomatteCoveragePollResult poll();

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
    explicit GpuCryptomatteCoverage(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuCryptomatteCoverageCreateResult final {
    std::unique_ptr<GpuCryptomatteCoverage> resampler;
    GpuCryptomatteCoverageDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return resampler != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
