#pragma once

// GPU-resident frame lease + owner registry.
//
// The runtime-owned preview display service produces a native, GPU-resident GpuDisplayImage on its
// owner thread. This class lets the UI/PreviewFrameCache hold only a cheap, copyable, opaque lease
// token instead of native ownership: the native GpuDisplayImage strong ownership never leaves the
// owner-bound registry except for one owner-thread native-present pin. Copying or dropping a token
// on any thread never touches Vulkan and never frees native memory. When the registry is destroyed,
// every surviving token safely reports invalid.
//
// The token's shared state carries only immutable geometry metadata (width, height, displayWindow,
// pixel aspect, actual allocation bytes), its lease id, the registry epoch, and one atomic validity
// flag. It holds no native image, no GpuDevice, and no Vulkan object. Actual ownership is
// distinguished by registry instance + epoch + token state, never by an image generation number.
//
// Budget is the sum of actual native allocation bytes for every active lease and every in-flight
// native-present pin. A publication that would exceed the budget is refused rather than silently
// invalidating an active UI lease. A lease that has been expired but is still pinned remains
// charged until its pin is released. All arithmetic is overflow-checked and the registry metadata
// count is bounded; there is no arbitrary global registry.
//
// Output qualification belongs to the eventual validating GPU product. This lease alone claims no
// ReferenceParity and publishes no display artifact.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::render {
class GpuDevice;
class GpuDisplayImage;
} // namespace bloom::render

namespace bloom::runtime {

// Implementation detail; complete type lives in gpu_resident_frame_lease.cpp. Never name this
// outside the implementation.
struct GpuResidentFrameLeaseState;
// Implementation detail; complete type lives in gpu_resident_frame_lease.cpp.
struct GpuResidentFrameLeaseEntry;

// A cheap, copyable, opaque lease token. It is safe to copy, move, and destroy on any thread,
// including the UI thread: it retains no native image, device, or Vulkan object. It is valid only
// while the owning registry still owns the lease; a token that outlives its registry reports
// false from isValid() and its metadata accessors remain safe.
class GpuResidentFrameLease final {
  public:
    GpuResidentFrameLease() noexcept;
    GpuResidentFrameLease(const GpuResidentFrameLease&) noexcept;
    GpuResidentFrameLease& operator=(const GpuResidentFrameLease&) noexcept;
    GpuResidentFrameLease(GpuResidentFrameLease&&) noexcept;
    GpuResidentFrameLease& operator=(GpuResidentFrameLease&&) noexcept;
    ~GpuResidentFrameLease();

    // Thread-safe. False for a default token, an invalidated lease, or a token whose registry has
    // been destroyed.
    [[nodiscard]] bool isValid() const noexcept;

    // Immutable metadata, safe to read on any thread and for an invalid token (defaults).
    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::optional<render::ImageWindow> displayWindow() const noexcept;
    [[nodiscard]] core::PixelAspectRatio pixelAspect() const noexcept;
    [[nodiscard]] std::uint64_t allocationBytes() const noexcept;
    [[nodiscard]] std::uint64_t registryEpoch() const noexcept;

  private:
    friend class GpuResidentFrameLeaseRegistry;
    explicit GpuResidentFrameLease(
        std::shared_ptr<const GpuResidentFrameLeaseState> state) noexcept;

    std::shared_ptr<const GpuResidentFrameLeaseState> state_;
};

// An owner-thread native-present pin: while it is alive it retains the actual native
// GpuDisplayImage strongly, so presentation can continue even after every UI lease has been
// invalidated. It is move-only. Destroying it on the owner thread releases only this pin; the
// registry entry (and its byte charge) is reclaimed by the next collectExpired() once no token and
// no pin remain.
//
// There is deliberately NO accessor that returns a strong shared_ptr<const GpuDisplayImage>. A
// strong copy handed to another owner would evade the registry's byte accounting (an untracked
// escape), so the present path must keep THIS pin alive through the presentation fence and consume
// `image()` as a borrowed reference, or wrap it in a non-owning alias that it guarantees cannot
// outlive the pin. See README §"Alias-safe native present" for the contract.
class GpuResidentFramePin final {
  public:
    GpuResidentFramePin() noexcept;
    GpuResidentFramePin(const GpuResidentFramePin&) = delete;
    GpuResidentFramePin& operator=(const GpuResidentFramePin&) = delete;
    GpuResidentFramePin(GpuResidentFramePin&& other) noexcept;
    GpuResidentFramePin& operator=(GpuResidentFramePin&& other) noexcept;
    ~GpuResidentFramePin();

    [[nodiscard]] bool isValid() const noexcept;
    // Valid only while isValid(); the referenced image is owned by the registry/pin. Owner thread.
    [[nodiscard]] const render::GpuDisplayImage& image() const noexcept;
    [[nodiscard]] std::uint64_t leaseId() const noexcept;

  private:
    friend class GpuResidentFrameLeaseRegistry;
    std::shared_ptr<const render::GpuDisplayImage> image_;
    std::shared_ptr<GpuResidentFrameLeaseEntry> entry_;
};

enum class GpuResidentFrameLeaseCode : std::uint8_t {
    None,
    InvalidArgument,
    WrongThread,
    ForeignDevice,
    ForeignRegistry,
    StaleLease,
    OverBudget,
    TooManyLeases,
    DeviceUnavailable,
    ShuttingDown,
};

struct GpuResidentFrameLeaseDiagnostic final {
    GpuResidentFrameLeaseCode code = GpuResidentFrameLeaseCode::None;
    std::string message;

    friend bool operator==(const GpuResidentFrameLeaseDiagnostic&,
                           const GpuResidentFrameLeaseDiagnostic&) = default;
};

struct GpuResidentFrameLeasePublishResult final {
    GpuResidentFrameLease lease;
    GpuResidentFrameLeaseDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept {
        return diagnostic.code == GpuResidentFrameLeaseCode::None && lease.isValid();
    }
    explicit operator bool() const noexcept { return hasValue(); }
};

struct GpuResidentFramePinResult final {
    GpuResidentFramePin pin;
    GpuResidentFrameLeaseDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return pin.isValid(); }
    explicit operator bool() const noexcept { return hasValue(); }
};

// Bounded metadata cap and byte budget. `maxBytes` is charged in actual native allocation bytes
// and is the authoritative limit; `maxEntries` only bounds metadata, active and tombstoned, so the
// registry can never grow without bound. The default entry cap is a finite 4096 (not the ~32 a
// one-second RAM preview would suggest) because the actual byte budget, not the entry count, is
// what protects memory; the UI/service is expected to set both explicitly for its own cache.
struct GpuResidentFrameLeaseBudgets final {
    std::uint64_t maxBytes = 512ULL * 1024ULL * 1024ULL;
    std::size_t maxEntries = 4096;
};

// Owner-thread registry bound to one actual GpuDevice. All mutating operations are owner-thread
// only and never block on the UI or on native completion. The caller must keep the bound GpuDevice
// alive for the registry's lifetime.
class GpuResidentFrameLeaseRegistry final {
  public:
    GpuResidentFrameLeaseRegistry(const GpuResidentFrameLeaseRegistry&) = delete;
    GpuResidentFrameLeaseRegistry& operator=(const GpuResidentFrameLeaseRegistry&) = delete;
    GpuResidentFrameLeaseRegistry(GpuResidentFrameLeaseRegistry&&) = delete;
    GpuResidentFrameLeaseRegistry& operator=(GpuResidentFrameLeaseRegistry&&) = delete;
    ~GpuResidentFrameLeaseRegistry();

    // Returns nullptr for an invalid budget (zero bytes or zero entries), for a foreign thread, or
    // for a device that is not the caller's own Ready generation (moved-from/Unavailable/stub,
    // Initializing, Lost, or ShuttingDown). The device owner thread and Ready state are checked via
    // GpuDevice before any allocation or native call; there is no implicit thread adoption.
    [[nodiscard]] static std::unique_ptr<GpuResidentFrameLeaseRegistry>
    create(render::GpuDevice& device, const GpuResidentFrameLeaseBudgets& budgets = {});

    [[nodiscard]] bool isBoundTo(render::GpuDevice& device) const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] GpuResidentFrameLeaseBudgets budgets() const noexcept;

    // Currently charged actual native bytes (active leases plus pinned tombstones). Safe to read on
    // the owner thread.
    [[nodiscard]] std::uint64_t chargedBytes() const noexcept;
    // Metadata entries retained (active and tombstoned). Bounded by budgets().maxEntries.
    [[nodiscard]] std::size_t entryCount() const noexcept;

    // Owner-thread. Takes strong native ownership of one valid display image bound to this
    // registry's device and returns an opaque token. Refuses (without disturbing active leases) on
    // a foreign device, a wrong thread, an exhausted metadata cap, or an exhausted byte budget.
    [[nodiscard]] GpuResidentFrameLeasePublishResult
    publish(std::shared_ptr<const render::GpuDisplayImage> image);

    // Owner-thread native lookup. Validates registry instance + epoch + token state before
    // returning a pin; rejects a foreign-registry token, a stale/invalidated token, a wrong thread,
    // and an image that has become invalid. The returned pin retains the native image.
    [[nodiscard]] GpuResidentFramePinResult pin(const GpuResidentFrameLease& lease);

    // Owner-thread. Marks every logical lease invalid and drops native ownership for every entry
    // that has no live pin. Pins already returned remain valid until the caller retires them.
    void invalidateAll() noexcept;

    // Owner-thread, non-blocking. Reclaims entries whose token is gone and whose pins are released,
    // and drops the native image of any invalidated unpinned entry. May be deferred until the next
    // publish/poll/shutdown; UI token release never frees native memory by itself.
    void collectExpired() noexcept;

    // Optional owner-thread wake hook for an existing service loop. It is invoked after a state
    // change and must not reenter the registry synchronously. No second thread, event bus, or
    // framework is created.
    void setWakeCallback(std::function<void()> callback);

  private:
    struct Impl;
    explicit GpuResidentFrameLeaseRegistry(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::runtime
