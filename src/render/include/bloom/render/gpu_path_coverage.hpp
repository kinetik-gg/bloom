#pragma once

// Bloom-owned, Qt-free and Vulkan-free bounded GPU coverage producer for vector
// paths. It consumes the immutable PathRasterCoverageGeometry (host-built
// scanline spans, never a CPU pixel mask) and dispatches one compute kernel that
// turns those spans into the exact 8-bit coverage mask the CPU PathRaster
// produces. The mask is retained in device memory as a packed little-endian R8
// storage buffer so a downstream GpuSolid covered fill can consume it directly,
// with no download or re-upload.
//
// The kernel does only integer sample-overlap and the CPU quantization
// `(count * 255 + 8) / 16`; it needs no Float64 or Int64 capability. One
// pipeline set per device, one outstanding job, owner-thread
// begin/poll/readback/cancel and destruction (matching the other render GPU
// operations). Wrong-thread calls fail closed without mutating owned state.
//
// readback() exists only for parity tests and the CPU oracle. The production
// path keeps the coverage resident and hands its buffer to GpuSolid.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bloom::render {

enum class GpuPathCoverageJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuPathCoveragePollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuPathCoverageDiagnosticCode : std::uint8_t {
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

struct GpuPathCoverageDiagnostic final {
    GpuPathCoverageDiagnosticCode code = GpuPathCoverageDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuPathCoverageDiagnostic&,
                           const GpuPathCoverageDiagnostic&) = default;
};

// The destination window and untouched display/PAR metadata. The fill rule,
// stroke alignment and clip are already baked into the supplied geometry.
struct GpuPathCoverageParameters final {
    ImageWindow dataWindow;
    ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
};

// Explicit caller allowances. These are the configured upper bounds for the uploaded geometry and
// the retained mask, not an artificial fixed ceiling: create() accepts any nonzero value and begin()
// additionally checks the genuine device storage-buffer range and the 32-bit span/word index bounds.
// The per-call byteBudget is a tighter allowance and must cover both.
struct GpuPathCoverageBudgets final {
    std::uint64_t maxGeometryBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maxCoverageBytes = 256ULL * 1024ULL * 1024ULL;
};

struct GpuPathCoverageCreateResult;
struct GpuPathCoverageImpl;
class GpuPathCoverage;
enum class GpuPathCoverageReadbackCode : std::uint8_t;
struct GpuPathCoverageReadback;
// Opaque shared ownership of the device-resident packed R8 coverage buffer. Only
// the narrow internal accessor below hands it to another render operation; no
// native type is exposed here.
struct GpuPathCoverageMaskOwner;
[[nodiscard]] std::shared_ptr<GpuPathCoverageMaskOwner>
gpuPathCoverageMask(const GpuPathCoverage& coverage) noexcept;

class GpuPathCoverage final {
  public:
    GpuPathCoverage(const GpuPathCoverage&) = delete;
    GpuPathCoverage& operator=(const GpuPathCoverage&) = delete;
    GpuPathCoverage(GpuPathCoverage&& other) noexcept;
    GpuPathCoverage& operator=(GpuPathCoverage&& other) noexcept;
    ~GpuPathCoverage();

    [[nodiscard]] static GpuPathCoverageCreateResult
    create(GpuDevice& device, const GpuPathCoverageBudgets& budgets = {});

    [[nodiscard]] GpuPathCoverageJobState state() const noexcept;
    [[nodiscard]] const GpuPathCoverageDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;
    // Width/height of the resident mask while Ready; 0 otherwise.
    [[nodiscard]] std::uint32_t coverageWidth() const noexcept;
    [[nodiscard]] std::uint32_t coverageHeight() const noexcept;
    // Positive proof that a native compute dispatch was submitted to the queue.
    [[nodiscard]] static std::uint64_t nativeDispatchCount() noexcept;

    // Validates the geometry window/entry bounds and budget, uploads the bounded
    // geometry, records and submits one dispatch. Returns None when accepted.
    [[nodiscard]] GpuPathCoverageDiagnostic
    begin(const GpuPathCoverageParameters& parameters, const PathRasterCoverageGeometry& geometry,
          std::uint64_t byteBudget);

    [[nodiscard]] GpuPathCoveragePollResult poll();

    // Test/oracle-only unpacked readback of the resident mask. Never used by the
    // normal path; the production consumer binds the resident buffer directly.
    [[nodiscard]] GpuPathCoverageReadback readback(std::uint64_t byteBudget) noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence
    // retires.
    void cancel() noexcept;

    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    explicit GpuPathCoverage(std::unique_ptr<GpuPathCoverageImpl> impl) noexcept;
    void releaseImpl() noexcept;
    friend std::shared_ptr<GpuPathCoverageMaskOwner>
    gpuPathCoverageMask(const GpuPathCoverage& coverage) noexcept;

    std::unique_ptr<GpuPathCoverageImpl> impl_;
};

enum class GpuPathCoverageReadbackCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    OverBudget,
    ReadbackFailed,
};

struct GpuPathCoverageReadback final {
    GpuPathCoverageReadbackCode code = GpuPathCoverageReadbackCode::None;
    std::string message;
    // One coverage byte per pixel, row-major over the data window.
    std::vector<std::uint8_t> coverage;

    [[nodiscard]] bool hasValue() const noexcept {
        return code == GpuPathCoverageReadbackCode::None;
    }
    explicit operator bool() const noexcept { return hasValue(); }
};

struct GpuPathCoverageCreateResult final {
    std::unique_ptr<GpuPathCoverage> coverage;
    GpuPathCoverageDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return coverage != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
