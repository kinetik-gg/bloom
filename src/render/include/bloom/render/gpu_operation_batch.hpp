#pragma once

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_solid.hpp>

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::render {
inline constexpr std::uint32_t kGpuOperationBatchCapacity = 8;
inline constexpr std::uint32_t kGpuBatchExternalImage = UINT32_MAX;
// Exactly one source: an already-retired external image, or an earlier operation's output.
// Pending internal outputs never escape the batch before its single completion fence proves all
// writes retired. References must be topological and bound to this exact device generation.
struct GpuBatchImageRef final {
    std::shared_ptr<const GpuImage> external;
    std::uint32_t operation = kGpuBatchExternalImage;
};
struct GpuBatchSolid final {
    GpuSolidParameters parameters;
};
struct GpuBatchGradient final {
    GpuBatchGradient(ImageWindow data, ImageWindow display, core::PixelAspectRatio aspect)
        : dataWindow(data), displayWindow(display), pixelAspect(aspect) {}
    ImageWindow dataWindow;
    ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::array<float, 4> first{}; // straight scene-linear RGBA
    std::array<float, 4> second{};
    std::array<float, 4> endpoints{}; // startX, startY, endX, endY in author pixels
    float inverseHorizontalScale = 1.0F;
    float inverseVerticalScale = 1.0F;
    float exactWidth = 0.0F;
    float exactHeight = 0.0F;
    std::uint32_t repeat = 0; // clamp, repeat, mirror
    bool radial = false;
};
struct GpuBatchTranslation final {
    GpuBatchImageRef source;
    ImageWindow outputWindow;
    double translationX;
    double translationY;
    float opacity;
};
struct GpuBatchAffine final {
    GpuBatchImageRef source;
    ImageWindow outputWindow;
    GpuAffineMatrix matrix;
    float opacity;
    std::optional<Rgba32fImageDescriptor> outputDescriptor{};
};
struct GpuBatchSourceOver final {
    GpuBatchImageRef source;
    GpuBatchImageRef destination;
};
using GpuBatchOperation =
    std::variant<GpuBatchSolid, GpuBatchTranslation, GpuBatchAffine, GpuBatchSourceOver,
                 GpuBatchGradient>;

enum class GpuOperationBatchCode : std::uint8_t {
    None,
    Unsupported,
    InvalidRequest,
    OverBudget,
    Busy,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    AllocationFailed,
    StatusRejected,
    Cancelled,
    Unproven,
};
struct GpuOperationBatchDiagnostic final {
    GpuOperationBatchCode code = GpuOperationBatchCode::None;
    std::string message;
};
enum class GpuOperationBatchState : std::uint8_t { Idle, Pending, Ready, Failure, WrongThread };
struct GpuOperationBatchStatistics final {
    std::uint64_t submissions = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t imageBytes = 0;
    std::uint64_t peakAllocationBytes = 0;
};
struct GpuOperationBatchBudgets final {
    std::uint64_t maxImageBytes = GpuSolidBudgets{}.maxImageBytes;
    std::uint64_t maxMetadataBytes = 256ULL * 1024 * 1024;
};

// One exclusive bounded native batch owner per device. Other facades report Busy for serial
// fallback. Uses the existing compute kernels and allocation helpers. All actual backing is
// admitted before one queue submission; one fence retires every input/output/metadata reference. No
// output escapes before every kernel status passes after genuine fence proof. Pre-submit refusal
// destroys backing before quiet same-origin prefix restoration, permitting generic native execution
// without a second allowance. Unknown retirement retains the full owner.
class GpuOperationBatch final {
  public:
    [[nodiscard]] static std::unique_ptr<GpuOperationBatch>
    create(GpuDevice&, const GpuOperationBatchBudgets& = {}) noexcept;
    GpuOperationBatch(const GpuOperationBatch&) = delete;
    GpuOperationBatch& operator=(const GpuOperationBatch&) = delete;
    GpuOperationBatch(GpuOperationBatch&&) noexcept;
    GpuOperationBatch& operator=(GpuOperationBatch&&) noexcept;
    ~GpuOperationBatch();

    [[nodiscard]] GpuOperationBatchDiagnostic begin(std::span<const GpuBatchOperation>,
                                                    std::uint64_t byteBudget);
    [[nodiscard]] GpuOperationBatchState poll() noexcept;
    [[nodiscard]] GpuOperationBatchState state() const noexcept;
    [[nodiscard]] GpuOperationBatchDiagnostic diagnostic() const;
    [[nodiscard]] GpuOperationBatchStatistics statistics() const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;
    [[nodiscard]] std::vector<GpuImage> takeImages();
    void cancel() noexcept;

  private:
    struct Impl;
    explicit GpuOperationBatch(std::unique_ptr<Impl>) noexcept;
    void releaseImpl() noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace bloom::render
