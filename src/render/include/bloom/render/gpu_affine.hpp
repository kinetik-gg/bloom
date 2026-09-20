#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU affine image transform: AffineBilinearV1. Additive to
// gpu_composite.hpp; it does not change TranslationOpacityBilinearV1 or SourceOverV1.
//
// The operation resamples ONE opaque, device-bound source GpuImage through a complete composed
// affine placement into a new resident RGBA32F GpuImage, preserving the source display window and
// pixel aspect ratio. It covers rotation 0/90/arbitrary, uniform/nonuniform/negative scale, an
// authored anchor plus translation, and any precomposed parent matrix (including shear).
//
// The CPU primitives are the correctness oracle:
//   - render::layerTransformBilinearRow() (src/render/cpu_image_primitives.cpp), and
//   - the parented row (src/runtime/layer_parent_transform.hpp).
// GPU parity is held to the documented per-finite-component 2e-6 absolute-or-relative gate
// (docs/architecture/gpu-backend.md), never claimed bit-exact.
//
// SAMPLE METADATA. The general affine inverse map is uploaded as a compact O(1) set of binary64
// coefficients (see GpuAffineMap) and the per-pixel source-local coordinate is derived on the GPU
// with a portable error-free double-float reconstruction; the kernel performs the exact CPU
// floor/factor reduction and the bilinear gather plus the opacity multiply. No O(width*height)
// per-pixel host metadata loop runs on the successful path. The host never generates or resamples
// RGBA pixels, and the kernel needs neither shaderFloat64 nor a 64-bit integer type. The original
// per-pixel host preparation below (prepareAffineSamples/prepareAffineMatrixSamples) is retained as
// the exact CPU reference used by tests; it is not on the GPU path.
//
// This header is intentionally narrow. It does not create a device or a service; it consumes the
// existing GpuDevice and GpuImage. Native work runs on the device owner thread and fails closed
// from any other thread. One job is outstanding at a time.

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::render {

// One host-prepared per-output-pixel sample. `baseX`/`baseY` are the exact CPU floor() of the
// inverse-mapped source-local coordinate and `factorX`/`factorY` the exact CPU Float32 fraction.
// `baseX == INT_MIN` is the transparent sentinel: the CPU's `sample <= -1.0 || >= extent` branch on
// either axis. base+/-1 taps outside the source data window are transparent black.
struct GpuAffineSample final {
    std::int32_t baseX = 0;
    std::int32_t baseY = 0;
    float factorX = 0.0F;
    float factorY = 0.0F;

    friend bool operator==(const GpuAffineSample&, const GpuAffineSample&) noexcept = default;
};

// The transparent sentinel value of GpuAffineSample::baseX.
inline constexpr std::int32_t kGpuAffineTransparentBase = std::numeric_limits<std::int32_t>::min();

// A complete composed forward affine placement of the source: a source-local pixel-centre
// coordinate (0-based within the source data window, the convention LayerTransform::forwardMap()
// uses) maps to an absolute output/composition coordinate as
//     outX = a * localX + b * localY + tx
//     outY = c * localX + d * localY + ty
// It may carry shear, which a rotation/scale decomposition alone cannot express.
struct GpuAffineMatrix final {
    double a = 1.0;
    double b = 0.0;
    double tx = 0.0;
    double c = 0.0;
    double d = 1.0;
    double ty = 0.0;

    friend bool operator==(const GpuAffineMatrix&, const GpuAffineMatrix&) noexcept = default;
};

// Exact host inverse-map preparation for the render-owned LayerTransform oracle. For every output
// pixel centre in `outputWindow` (absolute composition coordinates) it calls
// LayerTransform::inverseMap() -- the same function layerTransformBilinearRow() uses -- and reduces
// the result to the exact CPU base/factor pair. The returned vector is row-major, outputWindow
// width times height. Pure host arithmetic; identical in the Vulkan and stub builds.
[[nodiscard]] std::vector<GpuAffineSample> prepareAffineSamples(const LayerTransform& transform,
                                                                ImageWindow outputWindow);

// Exact host inverse-map preparation for a composed matrix over `sourceWindow`. The composed
// forward matrix is inverted in Float64; a non-finite or singular (zero-determinant) matrix has no
// inverse and yields an all-transparent sample vector, the empty-layer result. Otherwise each
// output pixel centre is inverse-mapped and reduced exactly as the CPU row does.
[[nodiscard]] std::vector<GpuAffineSample> prepareAffineMatrixSamples(const GpuAffineMatrix& matrix,
                                                                      ImageWindow sourceWindow,
                                                                      ImageWindow outputWindow);

// The compact O(1) GPU sample map: the inverse affine placement restricted to the output pixel
// lattice, expressed on output-local integer indices so the kernel needs no output origin and no
// per-pixel host loop:
//   localX(column, row) = localXAtOrigin + stepXPerColumn * column + stepXPerRow * row
//   localY(column, row) = localYAtOrigin + stepYPerColumn * column + stepYPerRow * row
// `column`/`row` are 0-based data-window indices of `outputWindow`; the origin is folded into
// `localXAtOrigin`/`localYAtOrigin`. The six coefficients are derived from the CPU oracle sampled
// at the output origin and at one step along each axis, so the map tracks the same CPU oracle the
// render path uses. This is a tolerance-qualified compact approximation, not a bit-exact
// reconstruction: the probed-difference coefficients are not universally identical to the oracle's
// own per-pixel inverseMap() evaluation for arbitrary origins or near-cancelling coefficients. The
// kernel reconstructs each pixel in error-free two-Float32 (double-float, ~48 significand bits)
// arithmetic without shaderFloat64, and parity is verified across the full output window by the
// native gates to 2e-6 abs-or-rel on all four components with bit-exact alpha at the 0/1 endpoints.
// A collapsed (non-finite or
// zero-determinant) composed matrix yields the all-transparent sentinel map (both constants -2.0,
// all steps 0).
struct GpuAffineMap final {
    double localXAtOrigin = 0.0;
    double localYAtOrigin = 0.0;
    double stepXPerColumn = 1.0;
    double stepXPerRow = 0.0;
    double stepYPerColumn = 0.0;
    double stepYPerRow = 1.0;

    friend bool operator==(const GpuAffineMap&, const GpuAffineMap&) noexcept = default;
};

// Compact O(1) inverse-map preparation for the LayerTransform oracle: probes
// LayerTransform::inverseMap() at the output origin and one local pixel step along each axis. Pure
// host arithmetic; O(1) in the output extent. Identical in the Vulkan and stub builds.
[[nodiscard]] GpuAffineMap prepareAffineMap(const LayerTransform& transform,
                                            ImageWindow outputWindow);

// Compact O(1) inverse-map preparation for the composed-matrix form. Uses the same closed-form
// inverse as prepareAffineMatrixSamples; a non-finite or singular matrix yields the transparent
// sentinel map. O(1) in the output extent.
[[nodiscard]] GpuAffineMap prepareAffineMatrixMap(const GpuAffineMatrix& matrix,
                                                  ImageWindow outputWindow);

// Validated inputs for the LayerTransform form. `source` is shared immutable ownership retained for
// the job's lifetime. `outputWindow` is the output DATA window only; the output DISPLAY window and
// pixel aspect are preserved from the source. The opacity is LayerTransform::opacity(), already
// rounded once to Float32 by the CPU primitive.
struct GpuAffineParameters final {
    std::shared_ptr<const GpuImage> source;
    ImageWindow outputWindow;
    std::optional<LayerTransform> transform;
};

// Validated inputs for the composed-matrix form. `opacity` must be finite in [0, 1] and is used as
// the Float32 binding directly (the caller has already rounded it once).
struct GpuAffineMatrixParameters final {
    std::shared_ptr<const GpuImage> source;
    ImageWindow outputWindow;
    GpuAffineMatrix matrix;
    float opacity = 1.0F;
};

// Per-call ceilings. Defaults are effectively unbounded so the real bounds are the live device
// limits and the per-call byteBudget; no fixed 256 MiB or 4K ceiling is baked in. This matches
// GpuPointResampleBudgets and the full-resolution admission direction. A non-zero injected value
// proves the fail-closed budget path.
struct GpuAffineBudgets final {
    std::uint64_t maxImageBytes = std::numeric_limits<std::uint64_t>::max();
    // The compact inverse-affine metadata is O(1) (six binary64 coefficients); the practical
    // metadata bound is the per-call byteBudget, never a per-pixel O(width*height) array.
    std::uint64_t maxMetadataBytes = std::numeric_limits<std::uint64_t>::max();
};

enum class GpuAffineJobState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuAffinePollResult : std::uint8_t { Pending, Ready, Failure, WrongThread };

enum class GpuAffineDiagnosticCode : std::uint8_t {
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

struct GpuAffineDiagnostic final {
    GpuAffineDiagnosticCode code = GpuAffineDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuAffineDiagnostic&, const GpuAffineDiagnostic&) = default;
};

struct GpuAffineCreateResult;

class GpuAffine final {
  public:
    GpuAffine(const GpuAffine&) = delete;
    GpuAffine& operator=(const GpuAffine&) = delete;
    GpuAffine(GpuAffine&& other) noexcept;
    GpuAffine& operator=(GpuAffine&& other) noexcept;
    ~GpuAffine();

    // Builds the cached AffineBilinearV1 pipeline, layout, descriptor set, command pool, and fence
    // once, on the device owner thread. Wrong thread returns WrongThread.
    [[nodiscard]] static GpuAffineCreateResult create(GpuDevice& device,
                                                      const GpuAffineBudgets& budgets = {});

    [[nodiscard]] GpuAffineJobState state() const noexcept;
    [[nodiscard]] const GpuAffineDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] std::uint64_t lastJobAllocationBytes() const noexcept;

    // Validates geometry/budgets/device limits and the device generation, prepares the host sample
    // metadata, creates the resident output image, and submits one dispatch. None when accepted.
    [[nodiscard]] GpuAffineDiagnostic beginAffine(const GpuAffineParameters& parameters,
                                                  std::uint64_t byteBudget);
    [[nodiscard]] GpuAffineDiagnostic beginAffineMatrix(const GpuAffineMatrixParameters& parameters,
                                                        std::uint64_t byteBudget);

    // Non-blocking fence query. On Ready the status flag word has been checked; a nonzero flag
    // publishes Failure(StatusFlagRejected) instead of Ready.
    [[nodiscard]] GpuAffinePollResult poll();

    // The resident output image while Ready; nullptr otherwise. Ownership stays with the pipeline.
    [[nodiscard]] const GpuImage* image() const noexcept;

    // Moves the resident output image out and returns the pipeline to Idle. Only valid when Ready.
    [[nodiscard]] GpuImage takeImage() noexcept;

    // Marks an outstanding job's result discarded; resources stay until the fence retires.
    void cancel() noexcept;

    // Process-global reported limitation: teardown could not drain an in-flight affine job within
    // its bounded budget and retained (did not destroy) busy Vulkan objects.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    struct Impl;
    explicit GpuAffine(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuAffineCreateResult final {
    std::unique_ptr<GpuAffine> affine;
    GpuAffineDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return affine != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
