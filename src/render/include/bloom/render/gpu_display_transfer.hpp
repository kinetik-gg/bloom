#pragma once

// Production final-display RGBA8 transfer. No scene evaluation or RGBA32F process readback.
// Native methods are device-owner-thread operations. begin/poll/cancel never wait. A foreign final
// release of Pending hands its preowned resources to the existing slot without driver/free work;
// only the exact native owner may drain it. Other states require owner-thread destruction. Closed
// owner handoff remains explicitly incomplete. Unproved submissions retain backing until proof.

#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/packed_rgba8_storage.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render {

enum class GpuDisplayTransferState : std::uint8_t { Idle, Pending, Ready, Failure };
enum class GpuDisplayTransferCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    Busy,
    OverBudget,
    TransferFailed,
    Cancelled,
};

struct GpuDisplayTransferDiagnostic final {
    GpuDisplayTransferCode code = GpuDisplayTransferCode::None;
    std::string message;
};

struct GpuDisplayTransferAccounting final {
    std::uint64_t submissions = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t stagingAllocationBytes = 0;
    std::uint64_t hostStorageBytes = 0;
    // Concurrent staging + host capacity; source allocation is separately owned/admitted by caller.
    std::uint64_t peakTransferBytes = 0;
    std::uint64_t sourceAllocationBytes = 0;
};

// Transfer-specific reservation independent of scheduler attachment/terminalization. The read-only
// handle survives quarantine/destruction but never retains a native allocation by itself.
struct GpuDisplayTransferRetentionSnapshot final {
    std::uint64_t reservedBytes = 0;
    std::uint64_t sourceBytes = 0;
    std::uint64_t stagingBytes = 0;
    std::uint64_t hostBytes = 0;
};
class GpuDisplayTransferRetention final {
  public:
    struct State;
    [[nodiscard]] GpuDisplayTransferRetentionSnapshot snapshot() const noexcept;

  private:
    friend class GpuDisplayTransfer;
    explicit GpuDisplayTransferRetention(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

// A completed transfer, constructible only after the primitive proves its fence retired. Carries
// exact source geometry/device generation; reading packed pixels does not change their ownership.
class GpuDisplayTransferResult final {
  public:
    GpuDisplayTransferResult(GpuDisplayTransferResult&&) noexcept = default;
    GpuDisplayTransferResult& operator=(GpuDisplayTransferResult&&) noexcept = default;
    GpuDisplayTransferResult(const GpuDisplayTransferResult&) = delete;
    GpuDisplayTransferResult& operator=(const GpuDisplayTransferResult&) = delete;

    [[nodiscard]] const ImageWindow& dataWindow() const noexcept { return dataWindow_; }
    [[nodiscard]] const ImageWindow& displayWindow() const noexcept { return displayWindow_; }
    [[nodiscard]] core::PixelAspectRatio pixelAspect() const noexcept { return pixelAspect_; }
    [[nodiscard]] std::uint32_t deviceGeneration() const noexcept { return generation_; }
    [[nodiscard]] bool isBoundTo(GpuDevice& device) const noexcept;
    [[nodiscard]] std::span<const Rgba8> pixels() const noexcept { return pixels_.pixels(); }
    [[nodiscard]] PackedRgba8Ownership pixelOwnership() const noexcept {
        return pixels_.ownership();
    }
    [[nodiscard]] const GpuDisplayTransferAccounting& accounting() const noexcept {
        return accounting_;
    }
    [[nodiscard]] PackedRgba8Storage takePixels() && noexcept { return std::move(pixels_); }

  private:
    friend class GpuDisplayTransfer;
    GpuDisplayTransferResult(ImageWindow dataWindow, ImageWindow displayWindow,
                             core::PixelAspectRatio pixelAspect, std::uint32_t generation,
                             PackedRgba8Storage pixels, GpuDisplayTransferAccounting accounting,
                             std::weak_ptr<const void> deviceBinding) noexcept
        : dataWindow_(dataWindow), displayWindow_(displayWindow), pixelAspect_(pixelAspect),
          generation_(generation), pixels_(std::move(pixels)), accounting_(accounting),
          deviceBinding_(std::move(deviceBinding)) {}

    ImageWindow dataWindow_, displayWindow_;
    core::PixelAspectRatio pixelAspect_;
    std::uint32_t generation_ = 0;
    PackedRgba8Storage pixels_;
    GpuDisplayTransferAccounting accounting_;
    // Weak control-block identity only: host results never retain an allocator/device allocation.
    std::weak_ptr<const void> deviceBinding_;
};

class GpuDisplayTransfer final {
  public:
    GpuDisplayTransfer();
    ~GpuDisplayTransfer();
    GpuDisplayTransfer(const GpuDisplayTransfer&) = delete;
    GpuDisplayTransfer& operator=(const GpuDisplayTransfer&) = delete;

    // Source must be complete and bound to this owner/device. byteBudget admits actual rounded
    // staging plus concurrent eventual host capacity; source native bytes remain separately
    // charged.
    [[nodiscard]] bool
    begin(GpuDevice& device, std::shared_ptr<const GpuDisplayImage> image, std::uint64_t byteBudget,
          const std::shared_ptr<GpuProcessingPhaseLease>& hostPhase = {}) noexcept;
    [[nodiscard]] GpuDisplayTransferState poll() noexcept;
    // Exactly once in Ready; returns to Idle. No host result is published on cancellation/failure.
    [[nodiscard]] std::optional<GpuDisplayTransferResult> take() noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool reset() noexcept;
    [[nodiscard]] GpuDisplayTransferState state() const noexcept;
    [[nodiscard]] const GpuDisplayTransferDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] const GpuDisplayTransferAccounting& accounting() const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] bool isOwnerThread() const noexcept;
    [[nodiscard]] std::shared_ptr<const GpuDisplayTransferRetention> retention() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::render
