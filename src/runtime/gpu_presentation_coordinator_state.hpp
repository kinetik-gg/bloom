#pragma once

// Internal owner-side state for the presentation coordinator. Both the coordinator translation unit
// (construction, public methods, shutdown report) and the pump translation unit (the drive state
// machine) include this; it is never part of a public consumer's include set. It holds no Qt and no
// Vulkan type, and it reuses the frozen GpuDevice / lease-registry / presentation-target public
// API.

#include "gpu_presentation_coordinator_private.hpp"

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {
class GpuDevice;
}

namespace bloom::runtime {

class GpuResidentFrameLeaseRegistry;

namespace presentation_coordinator_detail {

// Fixed hard cap on retained unproven generations, shared by every coordinator in the process. A
// slot is RESERVED before a native target is admitted (never in a teardown path), so quarantine
// itself performs no allocation and cannot fail after ownership has moved. The backing array is
// allocated exactly once and deliberately never freed, so no static destructor runs
// Vulkan/Swapchain teardown. The fuse latches as soon as any generation is retained or the cap
// overflows; once latched every coordinator refuses new native targets and new coordinators are
// invalid.
inline constexpr std::size_t kMaxQuarantinedTargets = 8;

// Opaque reservation token: an index into the fixed, non-destructed slot store.
using QuarantineReservation = std::size_t;
inline constexpr QuarantineReservation kNoQuarantineReservation = static_cast<std::size_t>(-1);

// Allocates the fixed slot store once (non-throwing: returns false if allocation fails). Must be
// called from a non-teardown path (coordinator construction) before any target is admitted.
[[nodiscard]] bool quarantineEnsureStorage() noexcept;
// Atomically reserves one slot. False means the bounded process capacity is exhausted; the caller
// must refuse the attach (never create native state). Does not latch the fuse by itself.
[[nodiscard]] bool quarantineReserve(QuarantineReservation& reservation) noexcept;
// Returns an unused reservation (proven Retired/Rejected, or attach refused before native
// creation).
void quarantineRelease(QuarantineReservation reservation) noexcept;
// Atomically consumes a valid reservation and moves ownership of an unprovable generation into the
// non-destructed slot. The alias must strongly own the lease pin (see retainingAlias). Always
// succeeds for a live reservation, latches the fuse, and never destroys the target.
void quarantineCommit(QuarantineReservation reservation,
                      std::unique_ptr<render::GpuPresentationTarget> target,
                      std::shared_ptr<const render::GpuDisplayImage> alias) noexcept;
[[nodiscard]] bool quarantineFuseLatched() noexcept;
void latchQuarantineFuse() noexcept;
[[nodiscard]] std::size_t quarantinedCount() noexcept;
[[nodiscard]] bool quarantineCapReached() noexcept;
// Test seam: clears the fixed store between isolated cases in one process.
void resetQuarantineForTesting() noexcept;

[[nodiscard]] bool terminalState(GpuPresentationTargetState state) noexcept;

// Builds a shared alias to `pin`'s image whose deleter owns `pin`, so the resident image (and its
// registry byte charge) stays alive for as long as any copy of the alias exists, including the
// native presenter's retained presenter input. This is the only retaining escape; no strong
// `shared_ptr<const GpuDisplayImage>` accessor is added to the registry.
[[nodiscard]] std::shared_ptr<const render::GpuDisplayImage>
retainingAlias(std::shared_ptr<GpuResidentFramePin> pin);

[[nodiscard]] render::GpuPresentOverlay
overlayDescriptor(const std::shared_ptr<const GpuPresentationOverlay>& overlay) noexcept;

struct CoordinatorState {
    struct Entry final {
        GpuPresentationTargetId id = kInvalidPresentationTarget;
        GpuPresentationTargetState state = GpuPresentationTargetState::Attaching;
        bool counted = true;
        std::unique_ptr<render::GpuPresentationTarget> native;
        render::GpuPresentationTargetDescription description;
        render::GpuPresentationTargetInfo info{};
        std::optional<detail::GpuPresentationPendingUpdate> pendingUpdate;
        std::optional<detail::GpuPresentationPendingResize> pendingResize;
        bool retirePending = false;
        std::uint64_t appliedSequence = 0;
        std::uint64_t presentCount = 0;
        bool nativeAcquired = false;
        // Actual overlay bytes currently applied and charged in the mailbox.
        std::uint64_t activeOverlayBytes = 0;
        // Extracted pending overlay bytes still charged but not yet applied.
        std::uint64_t chargedPendingBytes = 0;
        // Conservative native-image retention. This alias strongly owns the lease pin through its
        // deleter (see retainingAlias), so the image stays alive even after this entry is erased.
        std::shared_ptr<const render::GpuDisplayImage> activeAlias;
        // Bounded quarantine slot reserved before native creation and consumed by quarantine or
        // released when retirement/rejection is proven. Guarantees teardown never allocates.
        QuarantineReservation reservation = kNoQuarantineReservation;
        render::GpuPresentationTargetCode lastCode =
            render::GpuPresentationTargetCode::PresentationUnavailable;
        std::string message;
        // The surface this entry owns, so ownership can be released exactly once when the native
        // target is proven gone (Retired) or was never created (Rejected).
        detail::GpuPresentationMailbox::SurfaceKey surfaceKey{};
    };

    CoordinatorState() = default;
    CoordinatorState(render::GpuDevice* deviceValue, GpuResidentFrameLeaseRegistry* registryValue,
                     GpuPresentationCoordinatorOptions optionsValue)
        : device(deviceValue), registry(registryValue), options(optionsValue) {}

    render::GpuDevice* device = nullptr;
    GpuResidentFrameLeaseRegistry* registry = nullptr;
    GpuPresentationCoordinatorOptions options;
    std::thread::id ownerThread;
    bool valid = false;
    bool shuttingDown = false;
    std::size_t shutdownPumps = 0;
    std::shared_ptr<detail::GpuPresentationMailbox> mailbox;
    std::shared_ptr<GpuPresentationClient> client;
    std::map<GpuPresentationTargetId, Entry> entries;

    [[nodiscard]] bool ownerThreadHere() const noexcept {
        return std::this_thread::get_id() == ownerThread;
    }

    void releaseActiveImage(Entry& entry) noexcept {
        // The alias owns the pin; dropping it releases the pinned resident image (and its byte
        // charge) on the owner thread.
        entry.activeAlias.reset();
    }

    void releaseOverlayCharge(Entry& entry) noexcept {
        if (entry.activeOverlayBytes != 0U || entry.chargedPendingBytes != 0U) {
            std::lock_guard lock(mailbox->mutex);
            mailbox->chargedOverlayBytes -= entry.activeOverlayBytes + entry.chargedPendingBytes;
        }
        entry.activeOverlayBytes = 0;
        entry.chargedPendingBytes = 0;
    }

    void publish(const Entry& entry, const GpuPresentationTargetState state,
                 const bool safeToDestroy, const render::GpuPresentationTargetCode code,
                 std::string message) {
        std::function<void()> wake;
        {
            std::lock_guard lock(mailbox->mutex);
            GpuPresentationTargetSnapshot snapshot;
            snapshot.id = entry.id;
            snapshot.state = state;
            snapshot.appliedSequence = entry.appliedSequence;
            snapshot.presentCount = entry.presentCount;
            snapshot.surfaceSafeToDestroy = safeToDestroy;
            snapshot.targetCode = code;
            snapshot.info = entry.info;
            snapshot.message = std::move(message);
            mailbox->snapshots[entry.id] = std::move(snapshot);
            if (terminalState(state)) {
                mailbox->slots.erase(entry.id);
            }
            wake = mailbox->wake;
        }
        if (wake) {
            wake();
        }
    }

    void releaseAdmission(Entry& entry) {
        if (!entry.counted) {
            return;
        }
        std::lock_guard lock(mailbox->mutex);
        if (mailbox->admittedTargets > 0U) {
            --mailbox->admittedTargets;
        }
        entry.counted = false;
    }

    // Releases the (epoch, surface bits) ownership so the surface may be re-attached. Called ONLY
    // when the native target is proven gone (Retired) or was never created (Rejected); a
    // Quarantined or Unproven target still references the surface and keeps ownership.
    void releaseSurfaceOwnership(Entry& entry) {
        if (entry.surfaceKey.surfaceBits == 0U) {
            return;
        }
        std::lock_guard lock(mailbox->mutex);
        const auto it = mailbox->ownedSurfaces.find(entry.surfaceKey);
        if (it != mailbox->ownedSurfaces.end() && it->second == entry.id) {
            mailbox->ownedSurfaces.erase(it);
        }
        entry.surfaceKey = {};
    }

    void quarantine(Entry& entry, std::string message) {
        entry.message = std::move(message);
        entry.lastCode = render::GpuPresentationTargetCode::DriverUnavailable;
        releaseOverlayCharge(entry);
        auto target = std::move(entry.native);
        auto alias = std::move(entry.activeAlias);
        entry.state = GpuPresentationTargetState::Quarantined;
        entry.nativeAcquired = false;
        releaseAdmission(entry);
        if (target != nullptr) {
            if (entry.reservation == kNoQuarantineReservation) {
                // Invariant: a native target always holds a reservation taken before creation. If
                // it is ever missing, latch the fuse and take a late reservation when one remains.
                latchQuarantineFuse();
                QuarantineReservation late = kNoQuarantineReservation;
                if (quarantineReserve(late)) {
                    entry.reservation = late;
                }
            }
            if (entry.reservation != kNoQuarantineReservation) {
                quarantineCommit(entry.reservation, std::move(target), std::move(alias));
                entry.reservation = kNoQuarantineReservation;
            } else {
                // Bounded capacity exhausted and no reservation: the generation must not be
                // destroyed while it may reference a UI surface. Leak the raw target deliberately
                // (never destroyed) and keep the process fuse latched so no further native state
                // accrues anywhere. Any presenter-retained alias owns the pin independently.
                static_cast<void>(target.release());
            }
        } else if (entry.reservation != kNoQuarantineReservation) {
            quarantineRelease(entry.reservation);
            entry.reservation = kNoQuarantineReservation;
        }
        publish(entry, GpuPresentationTargetState::Quarantined, false,
                render::GpuPresentationTargetCode::DriverUnavailable, entry.message);
    }

    // A record may be erased only when its native target is proven gone: Retired or Rejected with
    // no native target, no reservation, and no retained surface ownership. Quarantined/Unproven
    // records keep their surface alive and are never forgotten.
    [[nodiscard]] static bool forgettable(const Entry& entry) noexcept {
        return (entry.state == GpuPresentationTargetState::Retired ||
                entry.state == GpuPresentationTargetState::Rejected) &&
               entry.native == nullptr && entry.reservation == kNoQuarantineReservation;
    }

    void ingestRequests() {
        std::function<void()> wake;
        {
            std::lock_guard lock(mailbox->mutex);
            // Drain terminal acknowledgements. A request for a live/unproven target stays pending
            // so nothing is silently forgotten; it is retried on a later pump.
            for (auto it = mailbox->forgetRequested.begin();
                 it != mailbox->forgetRequested.end();) {
                const auto entryIt = entries.find(*it);
                if (entryIt == entries.end()) {
                    it = mailbox->forgetRequested.erase(it);
                    continue;
                }
                if (forgettable(entryIt->second)) {
                    entries.erase(entryIt);
                    it = mailbox->forgetRequested.erase(it);
                    continue;
                }
                ++it;
            }
            for (auto& [id, slot] : mailbox->slots) {
                if (slot.attachPending) {
                    slot.attachPending = false;
                    Entry entry;
                    entry.id = id;
                    entry.description.surface = slot.surface;
                    entry.description.width = slot.attachWidth;
                    entry.description.height = slot.attachHeight;
                    entry.surfaceKey = {slot.surface.epoch.value, slot.surface.surface_bits};
                    entries[id] = std::move(entry);
                    continue;
                }
                const auto entryIt = entries.find(id);
                if (entryIt == entries.end() || terminalState(entryIt->second.state)) {
                    continue;
                }
                Entry& entry = entryIt->second;
                if (slot.retirePending) {
                    slot.retirePending = false;
                    entry.retirePending = true;
                    if (slot.update.has_value()) {
                        mailbox->chargedOverlayBytes -= slot.pendingOverlayBytes;
                    }
                    slot.pendingOverlayBytes = 0;
                    slot.update.reset();
                    slot.resize.reset();
                    continue;
                }
                if (slot.resize.has_value()) {
                    entry.pendingResize = slot.resize;
                    slot.resize.reset();
                }
                if (slot.update.has_value()) {
                    // A newer update supersedes any extracted-but-unapplied one; release the older
                    // charge before adopting the newer (already-charged) bytes.
                    mailbox->chargedOverlayBytes -= entry.chargedPendingBytes;
                    entry.chargedPendingBytes = slot.pendingOverlayBytes;
                    slot.pendingOverlayBytes = 0;
                    entry.pendingUpdate = std::move(slot.update);
                    slot.update.reset();
                }
            }
            wake = mailbox->wake;
        }
        if (wake) {
            wake();
        }
    }

    void driveAttach(Entry& entry);
    void drivePresent(Entry& entry);
    void driveRetire(Entry& entry);
    void driveTarget(Entry& entry);

    // Owner-thread summary of retained targets. Never called off the owner thread.
    [[nodiscard]] GpuPresentationShutdownStatus computeShutdownStatus() const;
    // Publishes the owner summary into the mailbox for synchronized off-owner reads.
    void publishShutdownSnapshot();

    void pumpOnce();
};

} // namespace presentation_coordinator_detail
} // namespace bloom::runtime
