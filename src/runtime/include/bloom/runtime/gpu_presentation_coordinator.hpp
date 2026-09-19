#pragma once

// Typed runtime presentation coordinator: the single owner-thread bridge between the UI-created
// Wayland surface and the existing GpuPresentationTarget / GPU-resident lease machinery.
//
// It is a *coordinator*, not a thread, a service, a device, or a generic message bus. The existing
// GpuPreviewDisplayServiceCore owns one and polls it from the service loop it already runs. This
// header is deliberately Qt-free and Vulkan-free: the client carries no native object, only typed
// requests and statuses. Attach crosses an actual UI-created borrowed surface/extent as opaque
// integer bits plus the device presentation epoch; every native check and every driver call happens
// on the device owner thread.
//
// Lifetime contract (documented, not faked):
//   * The UI-created instance, window, and surface remain UI-owned. The UI must keep them alive
//     until the coordinator publishes Retired for that target (surfaceSafeToDestroy == true), or
//     until a Rejected status proves we never created a native target for that surface.
//   * After the owner is torn down the shared mailbox latches ownerGone; every port method fails
//     closed. The client holds a shared mailbox, never a raw owner pointer.
//   * Failure / unknown / device-loss are published as Quarantined or Unproven, never as a fake
//   safe
//     acknowledgement; the native target is retained rather than destroyed while it references a
//     surface the presentation engine may still read.
//
// The present update carries an opaque resident lease (no native image), the actual
// GpuPresentImageParams, and shared immutable premultiplied RGBA8 overlay bytes prepared off the UI
// thread. The owner is the only code that resolves the lease to a native image, and it keeps the
// GpuResidentFramePin alive together with a non-owning alias through the render fence (and
// conservatively through retirement). The alias never outlives the pin and the native image is
// never exposed to the UI.

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>
#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {
class GpuDevice;
}

namespace bloom::runtime {

class GpuPresentationCoordinator;
class GpuResidentFrameLeaseRegistry;

namespace detail {
struct GpuPresentationMailbox;
}

using GpuPresentationTargetId = std::uint64_t;
inline constexpr GpuPresentationTargetId kInvalidPresentationTarget = 0;

// One immutable, off-UI-prepared premultiplied RGBA8 overlay. `create` copies the bytes into shared
// immutable storage and validates every dimension with checked arithmetic. A null return means the
// overlay is malformed and the update must not be issued. The presenter uploads only when the token
// changes, so a stable token across composited frames avoids per-frame uploads.
class GpuPresentationOverlay final {
  public:
    GpuPresentationOverlay(const GpuPresentationOverlay&) = delete;
    GpuPresentationOverlay& operator=(const GpuPresentationOverlay&) = delete;

    [[nodiscard]] static std::shared_ptr<const GpuPresentationOverlay>
    create(std::vector<std::uint8_t> pixels, std::uint32_t width, std::uint32_t height,
           std::uint32_t rowStrideBytes, std::uint64_t token, std::uint64_t byteBudget) noexcept;

    [[nodiscard]] const std::uint8_t* pixels() const noexcept { return pixels_.data(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t rowStrideBytes() const noexcept { return rowStrideBytes_; }
    [[nodiscard]] std::uint64_t token() const noexcept { return token_; }
    [[nodiscard]] std::uint64_t byteSize() const noexcept { return byteSize_; }

  private:
    GpuPresentationOverlay(std::vector<std::uint8_t> pixels, std::uint32_t width,
                           std::uint32_t height, std::uint32_t rowStrideBytes,
                           std::uint64_t token) noexcept;
    std::vector<std::uint8_t> pixels_;
    std::uint64_t byteSize_ = 0;
    std::uint64_t token_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t rowStrideBytes_ = 0;
};

// One present update, prepared off the UI thread. `overlay` is optional (null = no overlay). The
// lease is an opaque token with no native object.
struct GpuPresentationUpdate final {
    GpuResidentFrameLease lease;
    render::GpuPresentImageParams params;
    std::shared_ptr<const GpuPresentationOverlay> overlay;
};

enum class GpuPresentationPortCode : std::uint8_t {
    // The request was admitted (and will be applied on a later owner pump).
    Accepted,
    // The request superseded an earlier unapplied request for the same target; the older one will
    // never be presented.
    Coalesced,
    // A caller error: malformed overlay, invalid lease token, zero/over-budget extent.
    Rejected,
    // The bounded target admission cap is full.
    TooManyTargets,
    // The same (device epoch, surface bits) is already owned by another live target. The existing
    // owner is not retired by this rejection, so the surface must NOT be treated as safe to
    // destroy.
    DuplicateSurface,
    // The overlay would exceed the owner's aggregate overlay budget.
    OverBudget,
    // No such target id is known to the owner.
    UnknownTarget,
    // The request sequence is not newer than one already accepted for that target.
    StaleSequence,
    // Admission is closed for this target (retiring, retired, quarantined).
    Closed,
    // Owner-wide shutdown refuses new targets/updates.
    ShuttingDown,
    // A process-wide native retirement could not be proven. Every coordinator in the process stops
    // admitting new targets/updates; no new native state may accrue until the process ends.
    QuarantineFused,
    // A requested terminal forget was refused: the target is live or its retirement is unproven, so
    // its surface ownership must stay retained and its record may not be erased.
    NotTerminal,
    // The owner has been torn down; the port fails closed.
    OwnerGone,
};

struct GpuPresentationPortResult final {
    GpuPresentationPortCode code = GpuPresentationPortCode::OwnerGone;
    GpuPresentationTargetId target = kInvalidPresentationTarget;
    std::uint64_t sequence = 0;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return code == GpuPresentationPortCode::Accepted ||
               code == GpuPresentationPortCode::Coalesced;
    }
};

enum class GpuPresentationTargetState : std::uint8_t {
    // Attach admitted; the native target has not been created yet.
    Attaching,
    Active,
    Resizing,
    Retiring,
    // The presentation engine proved retirement and the native target was destroyed.
    Retired,
    // The owner refused the attach (bad epoch/surface, cap, shutdown) before creating native state.
    Rejected,
    // Device loss or a hard present failure: the native target is retained and the surface must not
    // be destroyed until the device generation dies.
    Quarantined,
    // Shutdown could not prove retirement within the bounded drain; the native target is retained.
    Unproven,
    // No status is published for this id (unknown to the owner, or owner gone).
    Gone,
};

// One consistent read of a target's published state. Safe on any thread.
struct GpuPresentationTargetSnapshot final {
    GpuPresentationTargetId id = kInvalidPresentationTarget;
    GpuPresentationTargetState state = GpuPresentationTargetState::Gone;
    // Last update sequence whose present was submitted.
    std::uint64_t appliedSequence = 0;
    // Monotonic count of successful presents (for latest-only coalescing evidence).
    std::uint64_t presentCount = 0;
    // True only when the native target is proven gone (Retired, or Rejected before native creation)
    // so the UI may destroy its window and surface.
    bool surfaceSafeToDestroy = false;
    render::GpuPresentationTargetCode targetCode =
        render::GpuPresentationTargetCode::PresentationUnavailable;
    render::GpuPresentationTargetInfo info{};
    std::string message;
};

struct GpuPresentationShutdownStatus final {
    bool accepting = true;
    std::size_t activeTargets = 0;
    std::size_t retiredTargets = 0;
    std::size_t quarantinedTargets = 0;
    std::size_t unprovenTargets = 0;
    // All targets reached a terminal state within the bounded drain.
    bool drained = false;
    std::string message;
};

struct GpuPresentationCoordinatorOptions final {
    // Bounded admission cap across attaching/active/resizing/retiring targets.
    std::size_t maxTargets = 8;
    // Aggregate overlay bytes charged against all retained/pending overlays.
    std::uint64_t maxOverlayBytes = 64ULL * 1024ULL * 1024ULL;
    // Per-overlay byte budget.
    std::uint64_t maxOverlayBytesPerFrame = 16ULL * 1024ULL * 1024ULL;
    std::uint32_t maxTargetExtent = 16384;
    std::uint32_t maxOverlayExtent = 8192;
    // Bounded count of records still visible to status() (live targets plus proven-terminal records
    // awaiting forget()). When reached, new attaches are refused until forget() releases a proven
    // record. This bounds owner entries/snapshots even before an adapter calls forget().
    std::size_t maxRetainedTargets = 4096;
    // Bounded shutdown drain: number of owner pumps after which remaining targets are reported
    // Unproven rather than silently retained forever.
    std::size_t shutdownDrainPumps = 240;
};

// UI-side port. Holds only a shared mailbox; no native object and no raw owner pointer. All methods
// are safe to call after the owner is gone (they fail closed).
class GpuPresentationClient final {
  public:
    GpuPresentationClient() noexcept;
    ~GpuPresentationClient();
    GpuPresentationClient(const GpuPresentationClient&) = delete;
    GpuPresentationClient& operator=(const GpuPresentationClient&) = delete;
    GpuPresentationClient(GpuPresentationClient&&) noexcept;
    GpuPresentationClient& operator=(GpuPresentationClient&&) noexcept;

    // Borrowed instance view/epoch, published only while the owner generation is valid. Returns an
    // invalid view (valid == false) after owner teardown or when presentation is not Ready.
    [[nodiscard]] render::GpuBorrowedInstanceView instanceView() const;

    // Admits an attach for a UI-created borrowed surface and the requested client extent, and
    // returns a stable target id immediately. The owner validates epoch/thread/native support on
    // its next pump; poll status(id) until it leaves Attaching.
    [[nodiscard]] GpuPresentationPortResult attach(const render::GpuBorrowedSurface& surface,
                                                   std::uint32_t width, std::uint32_t height);

    // Latest-only present update. A newer update for the same target replaces any unapplied one.
    [[nodiscard]] GpuPresentationPortResult
    update(GpuPresentationTargetId target, std::uint64_t sequence, GpuPresentationUpdate update);

    // Latest-only resize. The owner waits for prior native work to retire before recreating.
    [[nodiscard]] GpuPresentationPortResult resize(GpuPresentationTargetId target,
                                                   std::uint64_t sequence, std::uint32_t width,
                                                   std::uint32_t height);

    // Closes admission for the target and asks the owner to retire/retire-prove it.
    [[nodiscard]] GpuPresentationPortResult retire(GpuPresentationTargetId target,
                                                   std::uint64_t sequence);

    // Drops the pending update if `sequence` matches it (0 matches any unapplied update).
    [[nodiscard]] GpuPresentationPortResult cancel(GpuPresentationTargetId target,
                                                   std::uint64_t sequence);

    // Bounded terminal acknowledgement: releases a PROVEN-terminal record (Retired with
    // surfaceSafeToDestroy, or Rejected before native creation) so the owner erases its retained
    // entry and snapshot. A live, retiring, quarantined, or otherwise unproven target is refused
    // with NotTerminal and keeps its surface ownership and record. Forgetting never reuses an id
    // (ids are monotonic), and without it the owner retains proven records only up to the bounded
    // option `maxRetainedTargets`, after which new attaches are refused rather than silently
    // dropped. The default service adapter does not call this yet; wiring it is a documented
    // follow-up hook.
    [[nodiscard]] GpuPresentationPortResult forget(GpuPresentationTargetId target);

    [[nodiscard]] GpuPresentationTargetSnapshot status(GpuPresentationTargetId target) const;
    [[nodiscard]] bool ownerAlive() const noexcept;

    // Test-only: builds a client over an externally-owned mailbox so the CPU-only port validation
    // can exercise coalescing/sequencing/budgets without a device or an owner. Production clients
    // come from GpuPresentationCoordinator::client().
    [[nodiscard]] static std::shared_ptr<GpuPresentationClient>
    createForTesting(std::shared_ptr<detail::GpuPresentationMailbox> mailbox);

  private:
    friend class GpuPresentationCoordinator;
    explicit GpuPresentationClient(
        std::shared_ptr<detail::GpuPresentationMailbox> mailbox) noexcept;

    std::shared_ptr<detail::GpuPresentationMailbox> mailbox_;
};

// Owner-thread coordinator. Constructed with the existing GpuDevice and
// GpuResidentFrameLeaseRegistry that already live on the service thread; it owns no second thread
// and starts no work until pump() is called. Destruction is owner-thread and fails the port closed
// first.
class GpuPresentationCoordinator final {
  public:
    GpuPresentationCoordinator(const GpuPresentationCoordinator&) = delete;
    GpuPresentationCoordinator& operator=(const GpuPresentationCoordinator&) = delete;
    GpuPresentationCoordinator(GpuPresentationCoordinator&&) = delete;
    GpuPresentationCoordinator& operator=(GpuPresentationCoordinator&&) = delete;

    // Both dependencies must outlive the coordinator.
    GpuPresentationCoordinator(render::GpuDevice& device, GpuResidentFrameLeaseRegistry& registry,
                               GpuPresentationCoordinatorOptions options = {});
    ~GpuPresentationCoordinator();

    [[nodiscard]] std::shared_ptr<GpuPresentationClient> client() const;

    // Optional single wake hook for the existing service loop. Invoked without holding internal
    // locks, from whichever thread enqueued work or published a state change. It must be
    // thread-safe, must not reenter the coordinator, and must not touch widgets.
    void setWakeCallback(std::function<void()> callback);

    // Non-blocking. Drains admitted requests and drives acquire/render/present/retire with no
    // waits.
    void pump();

    // Refuses new targets/updates and asks every target to retire. Non-blocking; keep calling
    // pump() until shutdownStatus().drained or the drain budget is exceeded.
    void beginShutdown() noexcept;

    // Safe from any thread. On the owner thread it computes and republishes a synchronized
    // snapshot; from the UI thread it returns the last owner-published snapshot without traversing
    // owner entries. A caller that never saw an owner pump sees a conservative not-drained
    // snapshot.
    [[nodiscard]] GpuPresentationShutdownStatus shutdownStatus() const;
    [[nodiscard]] bool isShuttingDown() const noexcept;

    // Process-global, non-blocking diagnostic: how many unprovable native generations are retained
    // in the bounded quarantine, and whether the retention cap has been reached. A nonzero retained
    // count means at least one UI surface must stay alive until the device generation ends.
    // Owner-thread safe; also safe to read from any thread.
    [[nodiscard]] static std::size_t quarantinedGenerationCount() noexcept;
    [[nodiscard]] static bool quarantineCapacityReached() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::runtime
