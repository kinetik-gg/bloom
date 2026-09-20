#pragma once

// Bloom-owned, Qt-free and Vulkan-free swapchain presentation target. It consumes a borrowed
// Wayland surface minted against a GpuDevice's borrowed instance view and owns the real swapchain
// lifecycle for that same device. No Vk* or Qt type is public: handles cross as integer bits and
// the presentation epoch is validated on the device owner thread before any driver call.
//
// This package proves the lifecycle only. It clears the acquired swapchain image to a color and
// presents; it does not sample the GPU-resident display image, and it draws no checkerboard,
// channel view, or overlays. Those are the next small package and are deliberately not claimed
// here.
//
// Retirement is bounded by a real presentation-engine signal, never by a render fence:
// VK_EXT/KHR_swapchain_maintenance1 present fences or VK_KHR_present_wait + VK_KHR_present_id. A
// render fence proves command completion only. When neither mechanism is enabled the target
// refuses to be created and the caller keeps the CPU path. There is no unbounded
// vkQueueWaitIdle/vkDeviceWaitIdle anywhere in this file, and the UI never waits.

#include <bloom/render/gpu_device.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bloom::render {

class GpuDisplayImage;
struct GpuPresentImageParams;
struct GpuPresentOverlay;

// Bloom-owned swapchain format choice. Bgra8Unorm with an SRGB_NONLINEAR color space is preferred
// because the display shader already emits sRGB-encoded bytes; that pairing avoids a second gamma
// encode. Bgra8Srgb is only a fallback when the UNORM format is not offered.
enum class GpuPresentationFormat : std::uint8_t {
    Unknown,
    Bgra8Unorm,
    Bgra8Srgb,
    Rgba8Unorm,
    Rgba8Srgb,
};

// How presentation completion is proven. None means no supported mechanism and therefore no target.
enum class GpuPresentationRetirement : std::uint8_t {
    None,
    PresentFence,
    PresentWait,
};

enum class GpuPresentationTargetCode : std::uint8_t {
    Ok,
    // No swapchain image was ready for this zero-timeout acquire; poll again later.
    NotReady,
    OutOfDate,
    Suboptimal,
    // present() had no acquired image, or an acquire was attempted while a present was outstanding.
    NothingAcquired,
    // Retirement is still draining; the native surface must not be destroyed yet.
    RetirePending,
    Retired,
    DeviceLost,
    // The device cannot present at all (no surface/swapchain/queue/retirement mechanism).
    PresentationUnavailable,
    NoRetirementMechanism,
    WrongThread,
    WrongEpoch,
    UnsupportedSurface,
    DriverUnavailable,
    InvalidArgument,
};

struct GpuClearColor final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;
};

struct GpuPresentationTargetInfo final {
    GpuPresentationFormat format = GpuPresentationFormat::Unknown;
    // True when the chosen color space is SRGB_NONLINEAR (the no-double-gamma pairing).
    bool srgb_nonlinear = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t image_count = 0;
    GpuPresentationRetirement retirement = GpuPresentationRetirement::None;
};

// A borrowed surface plus the requested client extent. The owner clamps the extent to the surface
// capabilities; a zero width/height asks for the current surface extent.
struct GpuPresentationTargetDescription final {
    GpuBorrowedSurface surface;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct GpuPresentationTargetResult;

// Owner-thread-only swapchain target. It is move-only and owns its swapchain, per-image
// semaphores/present fences, command pool, and command buffer. A non-owner call fails closed.
class GpuPresentationTarget final {
  public:
    GpuPresentationTarget(const GpuPresentationTarget&) = delete;
    GpuPresentationTarget& operator=(const GpuPresentationTarget&) = delete;
    GpuPresentationTarget(GpuPresentationTarget&&) noexcept;
    GpuPresentationTarget& operator=(GpuPresentationTarget&&) noexcept;
    ~GpuPresentationTarget();

    // Validates the borrowed surface on the device owner thread (epoch/thread/support) before any
    // driver call, requires presentationStatus() Ready, requires a retirement mechanism, then
    // creates the swapchain and per-image resources. Returns a typed code with no target on
    // failure.
    [[nodiscard]] static GpuPresentationTargetResult
    create(GpuDevice& device, const GpuPresentationTargetDescription& description);

    // Zero-timeout acquire. NotReady/OutOfDate/Suboptimal/DeviceLost are returned as codes; a
    // Suboptimal result still carries an acquired image so the caller can present and then
    // recreate.
    [[nodiscard]] GpuPresentationTargetCode acquire();

    // Clears the acquired image to `color` on the owner thread and presents it. Bounded to one
    // outstanding present; a second call while one is outstanding returns NothingAcquired.
    [[nodiscard]] GpuPresentationTargetCode present(GpuClearColor color);

    // Samples a resident display image (and optional premultiplied RGBA8 overlay) into the acquired
    // swapchain image and presents it. Implemented by the GPU present-image module, which owns the
    // fixed sampler pipeline; this target only supplies its acquired image and retirement
    // machinery. The display image stays resident (no readback, no full-frame upload) and must be
    // bound to this device generation. The target's UNORM format is required; an _SRGB attachment
    // is rejected because the resident bytes are already sRGB-encoded. Owner-thread only.
    // Strong-ownership entry point: the target pins the shared resident image until its render
    // fence is known complete (or the target is quarantined), so the caller may release its own
    // reference once this returns.
    [[nodiscard]] GpuPresentationTargetCode
    presentImage(const std::shared_ptr<const GpuDisplayImage>& input,
                 const GpuPresentImageParams& params, const GpuPresentOverlay& overlay);

    // Non-blocking retirement progress. Never blocks and never waits idle. Returns Retired only
    // when every present is proven complete by the presentation engine and every render submit has
    // completed, so the native surface is safe for Qt to destroy. On an unknown/unsupported result
    // it retains the native resources and returns RetirePending so the retire ack is withheld.
    [[nodiscard]] GpuPresentationTargetCode pollRetirement();

    // Recreates the swapchain after OutOfDate/Suboptimal. Only valid when no acquire/present/render
    // is outstanding; reuses the borrowed surface/epoch after revalidation. Non-blocking.
    [[nodiscard]] GpuPresentationTargetCode
    recreate(const GpuPresentationTargetDescription& description);

    // Begins retirement and returns the current state. Retired means the caller may destroy the
    // native window/surface; RetirePending (including an unknown driver result) means it must not.
    // DeviceLost is terminal and never reports Retired.
    [[nodiscard]] GpuPresentationTargetCode beginRetire();

    [[nodiscard]] GpuPresentationTargetCode retireState() const noexcept;

    // Process-global report that a native target could not prove its presentation retired and was
    // quarantined rather than destroyed on a foreign thread or an unproven submission. Consistent
    // with GpuNeutralDisplay/GpuImage teardown reporting.
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;
    [[nodiscard]] GpuPresentationTargetInfo info() const noexcept;
    [[nodiscard]] std::uint32_t acquiredImageIndex() const noexcept;
    [[nodiscard]] GpuPresentationTargetCode lastCode() const noexcept;
    [[nodiscard]] const std::string& lastMessage() const noexcept;

  private:
    struct Impl;
    explicit GpuPresentationTarget(std::unique_ptr<Impl> impl) noexcept;
    // Owner-thread bounded poll, or bounded process quarantine on a foreign thread / unproven
    // submission. Never calls Vulkan from a foreign thread.
    void releaseImpl() noexcept;
    std::unique_ptr<Impl> impl_;
};

struct GpuPresentationTargetResult final {
    std::unique_ptr<GpuPresentationTarget> target;
    GpuPresentationTargetCode code = GpuPresentationTargetCode::PresentationUnavailable;
    std::string message;

    [[nodiscard]] bool hasValue() const noexcept { return target != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::render
