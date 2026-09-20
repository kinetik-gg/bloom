#pragma once

// Private transport for the presentation coordinator. It carries only UI-side typed requests and
// owner-published snapshots; it holds no native object. The owner drains it on its pump; the client
// mutates it under the mailbox mutex. Bounded by construction: at most one attach slot, one pending
// update, one pending resize, and one retire flag per admitted target, and admission is capped.

#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace bloom::runtime::presentation_coordinator_detail {

// Process-wide latch: a native generation could not be proven retired. Defined by the owner pump;
// the UI-side port consults it so a fused process admits no further native targets anywhere.
[[nodiscard]] bool quarantineFuseLatched() noexcept;

} // namespace bloom::runtime::presentation_coordinator_detail

namespace bloom::runtime::detail {

struct GpuPresentationPendingUpdate final {
    GpuResidentFrameLease lease;
    render::GpuPresentImageParams params;
    std::shared_ptr<const GpuPresentationOverlay> overlay;
    std::uint64_t sequence = 0;
    std::uint64_t overlayBytes = 0;
};

struct GpuPresentationPendingResize final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t sequence = 0;
};

// UI-authored request slot for one target id. Exactly one of each kind is retained; a newer update
// replaces the older one (latest-only coalescing), so no present backlog can accumulate in the
// port.
struct GpuPresentationClientSlot final {
    bool attachPending = false;
    render::GpuBorrowedSurface surface;
    std::uint32_t attachWidth = 0;
    std::uint32_t attachHeight = 0;

    std::optional<GpuPresentationPendingUpdate> update;
    std::optional<GpuPresentationPendingResize> resize;
    bool retirePending = false;
    std::uint64_t retireSequence = 0;

    // Overlay bytes currently charged for `update` (0 when none). Kept so a replacement update can
    // release the old charge without walking the owner's retained state.
    std::uint64_t pendingOverlayBytes = 0;

    // Set true the moment retire is admitted; closes update/resize/retire admission synchronously,
    // independent of the owner's published snapshot (which may not have been pumped yet).
    bool admissionClosed = false;

    std::uint64_t highestSequence = 0;
};

struct GpuPresentationMailbox final {
    mutable std::mutex mutex;

    // Latched by the owner destructor before releasing native state; fails every port method
    // closed.
    bool ownerGone = false;
    // Cleared by beginShutdown(); refuses new attaches and updates.
    bool accepting = true;

    render::GpuBorrowedInstanceView view;
    std::atomic<std::uint64_t> nextTargetId{1};
    std::size_t maxTargets = 8;
    std::size_t admittedTargets = 0;
    // Bounded count of retained records (live + proven terminal awaiting forget). New attaches are
    // refused at this bound so entries/snapshots cannot grow without an adapter calling forget().
    std::size_t maxRetainedRecords = 4096;

    // Aggregate overlay bytes currently retained by the port/owner (pending + applied). Charged in
    // actual bytes and bounded by maxOverlayBytes.
    std::uint64_t chargedOverlayBytes = 0;
    std::uint64_t maxOverlayBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maxOverlayBytesPerFrame = 16ULL * 1024ULL * 1024ULL;
    std::uint32_t maxTargetExtent = 16384;
    std::uint32_t maxOverlayExtent = 8192;

    // Requests from the UI thread. Drained (swapped) by the owner on every pump.
    std::map<GpuPresentationTargetId, GpuPresentationClientSlot> slots;
    // Owner-published snapshots read by status().
    std::map<GpuPresentationTargetId, GpuPresentationTargetSnapshot> snapshots;

    // Live ownership of a borrowed surface, keyed by (device presentation epoch, surface bits). A
    // surface already owned by an Attaching/Active/Resizing/Retiring/Quarantined target must never
    // be attached a second time: two non-retired swapchains over one Wayland surface is invalid.
    // The entry is removed only when the owning target is proven Retired or Rejected (never owned),
    // so a surface may be re-attached only after its native target is actually gone.
    struct SurfaceKey final {
        std::uint64_t epoch = 0;
        std::uint64_t surfaceBits = 0;
        friend bool operator<(const SurfaceKey& lhs, const SurfaceKey& rhs) noexcept {
            if (lhs.epoch != rhs.epoch) {
                return lhs.epoch < rhs.epoch;
            }
            return lhs.surfaceBits < rhs.surfaceBits;
        }
    };
    std::map<SurfaceKey, GpuPresentationTargetId> ownedSurfaces;

    // Terminal acknowledgements requested by the UI. The owner drains these and erases the matching
    // entry ONLY when the record is proven terminal-safe; a live/unproven request stays pending.
    std::set<GpuPresentationTargetId> forgetRequested;

    // Owner-published shutdown summary so status can be read from the UI thread without traversing
    // owner entries. Written under `mutex` on every owner pump.
    GpuPresentationShutdownStatus shutdownSnapshot;
    bool shutdownSnapshotValid = false;

    std::function<void()> wake;
};

} // namespace bloom::runtime::detail
