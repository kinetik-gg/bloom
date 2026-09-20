// Owner-thread pump and bounded-quarantine implementation for the presentation coordinator. It owns
// the drive state machine (attach/present/retire), overlay/native resource accounting, and the
// fixed process quarantine. It contains no UI code and never touches Qt or a Vk* type directly.

#include "gpu_presentation_coordinator_state.hpp"

#include <bloom/render/gpu_device.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime::presentation_coordinator_detail {

using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationOverlay;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuPresentationUpdate;
using bloom::runtime::detail::GpuPresentationPendingResize;
using bloom::runtime::detail::GpuPresentationPendingUpdate;

// --- Bounded process quarantine -----------------------------------------------------------------
// A target that cannot be proven retired (device loss, unknown present result, shutdown budget
// exhausted) must not be destroyed while it may still reference a UI-owned surface. Slots are
// RESERVED before a native target is created and consumed at quarantine time, so retention performs
// no allocation and never throws. The backing array is allocated once from the (non-teardown)
// coordinator constructor and deliberately never freed, so no destructor (and no Vulkan/Swapchain
// teardown) runs at static exit. The fuse latches on any retention or on overflow; a latched fuse
// stops new native targets in every coordinator, not only newly constructed ones.
namespace {

struct QuarantineSlot final {
    enum class State : std::uint8_t { Free, Reserved, Committed };
    State state = State::Free;
    // Raw, never-destroyed generation: a raw pointer so no destructor can run on the retained
    // target.
    render::GpuPresentationTarget* target = nullptr;
    // Retaining alias that strongly owns the lease pin, so the resident image stays alive.
    std::shared_ptr<const render::GpuDisplayImage> alias;
};

struct QuarantineStore final {
    std::mutex mutex;
    QuarantineSlot* slots = nullptr; // new[] once; never freed
    std::size_t capacity = 0;
    std::size_t reserved = 0;  // Reserved + Committed slots
    std::size_t committed = 0; // retained unprovable generations
    std::atomic_bool fuse{false};
};

QuarantineStore& quarantineStore() {
    static QuarantineStore store;
    return store;
}

} // namespace

[[nodiscard]] bool quarantineEnsureStorage() noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    if (store.slots != nullptr) {
        return true;
    }
    try {
        store.slots = new QuarantineSlot[kMaxQuarantinedTargets];
    } catch (...) {
        store.capacity = 0;
        store.fuse.store(true, std::memory_order_release);
        return false;
    }
    store.capacity = kMaxQuarantinedTargets;
    return true;
}

[[nodiscard]] bool quarantineReserve(QuarantineReservation& reservation) noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    reservation = kNoQuarantineReservation;
    if (store.slots == nullptr || store.fuse.load(std::memory_order_acquire)) {
        return false;
    }
    for (std::size_t index = 0; index < store.capacity; ++index) {
        if (store.slots[index].state == QuarantineSlot::State::Free) {
            store.slots[index].state = QuarantineSlot::State::Reserved;
            ++store.reserved;
            reservation = index;
            return true;
        }
    }
    // Capacity is momentarily exhausted because healthy targets hold every reservation. This is
    // ordinary back-pressure, NOT proof that a generation is unretirable: refuse this attach
    // (temporarily) but do NOT latch the process fuse, so a later attach succeeds as soon as a
    // healthy reservation is released. The irreversible fuse is latched only by an actual unproven
    // retention (quarantineCommit) or an allocation failure, never by opening one target too many.
    return false;
}

void quarantineRelease(QuarantineReservation reservation) noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    if (reservation >= store.capacity || store.slots == nullptr) {
        return;
    }
    QuarantineSlot& slot = store.slots[reservation];
    if (slot.state != QuarantineSlot::State::Reserved) {
        return; // A committed slot is never released.
    }
    slot.state = QuarantineSlot::State::Free;
    slot.target = nullptr;
    slot.alias.reset();
    if (store.reserved > 0) {
        --store.reserved;
    }
}

void quarantineCommit(QuarantineReservation reservation,
                      std::unique_ptr<render::GpuPresentationTarget> target,
                      std::shared_ptr<const render::GpuDisplayImage> alias) noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    store.fuse.store(true, std::memory_order_release);
    if (reservation >= store.capacity || store.slots == nullptr) {
        // No slot: leak the raw generation rather than destroying a possibly-referenced surface.
        static_cast<void>(target.release()); // NOLINT(bugprone-unused-return-value)
        return;
    }
    QuarantineSlot& slot = store.slots[reservation];
    if (slot.state != QuarantineSlot::State::Reserved) {
        static_cast<void>(target.release()); // NOLINT(bugprone-unused-return-value)
        return;
    }
    slot.target = target.release();
    slot.alias = std::move(alias);
    slot.state = QuarantineSlot::State::Committed;
    ++store.committed;
}

[[nodiscard]] bool quarantineFuseLatched() noexcept {
    return quarantineStore().fuse.load(std::memory_order_acquire);
}

void latchQuarantineFuse() noexcept {
    quarantineStore().fuse.store(true, std::memory_order_release);
}

[[nodiscard]] std::size_t quarantinedCount() noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    return store.committed;
}

[[nodiscard]] bool quarantineCapReached() noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    return store.slots != nullptr && store.reserved >= store.capacity;
}

void resetQuarantineForTesting() noexcept {
    QuarantineStore& store = quarantineStore();
    std::lock_guard lock(store.mutex);
    // Test-only reset MUST NOT drop an actual retained generation: its alias strongly owns a live
    // lease pin and its raw target may still reference a UI surface the presentation engine is
    // reading. Refuse to reset while any committed slot holds a real target or alias; the fixture
    // must prove it holds no real resources first.
    for (std::size_t index = 0; index < store.capacity; ++index) {
        if (store.slots != nullptr &&
            store.slots[index].state == QuarantineSlot::State::Committed &&
            (store.slots[index].target != nullptr || store.slots[index].alias != nullptr)) {
            return;
        }
    }
    if (store.slots == nullptr) {
        store.fuse.store(false, std::memory_order_release);
        return;
    }
    for (std::size_t index = 0; index < store.capacity; ++index) {
        store.slots[index].state = QuarantineSlot::State::Free;
        store.slots[index].target = nullptr;
        store.slots[index].alias.reset();
    }
    store.reserved = 0;
    store.committed = 0;
    store.fuse.store(false, std::memory_order_release);
}

[[nodiscard]] std::shared_ptr<const render::GpuDisplayImage>
retainingAlias(std::shared_ptr<GpuResidentFramePin> pin) {
    if (pin == nullptr || !pin->isValid()) {
        return nullptr;
    }
    const render::GpuDisplayImage* image = &pin->image();
    return std::shared_ptr<const render::GpuDisplayImage>(
        image, [holder = std::move(pin)](const render::GpuDisplayImage*) noexcept {});
}

// --- Helpers ------------------------------------------------------------------------------------

[[nodiscard]] bool terminalState(const GpuPresentationTargetState state) noexcept {
    switch (state) {
    case GpuPresentationTargetState::Retired:
    case GpuPresentationTargetState::Rejected:
    case GpuPresentationTargetState::Quarantined:
    case GpuPresentationTargetState::Unproven:
    case GpuPresentationTargetState::Gone:
        return true;
    case GpuPresentationTargetState::Attaching:
    case GpuPresentationTargetState::Active:
    case GpuPresentationTargetState::Resizing:
    case GpuPresentationTargetState::Retiring:
        return false;
    }
    return true;
}

[[nodiscard]] render::GpuPresentOverlay
overlayDescriptor(const std::shared_ptr<const GpuPresentationOverlay>& overlay) noexcept {
    render::GpuPresentOverlay descriptor;
    if (overlay == nullptr) {
        return descriptor;
    }
    descriptor.pixels = overlay->pixels();
    descriptor.width = overlay->width();
    descriptor.height = overlay->height();
    descriptor.rowStrideBytes = overlay->rowStrideBytes();
    descriptor.premultiplied = true;
    descriptor.token = overlay->token();
    return descriptor;
}

// --- Drive state machine
// --------------------------------------------------------------------------

void CoordinatorState::driveAttach(Entry& entry) {
    if (entry.retirePending || shuttingDown) {
        // No native target was ever created for this surface, so the UI may destroy it immediately.
        entry.state = GpuPresentationTargetState::Retired;
        releaseOverlayCharge(entry);
        releaseSurfaceOwnership(entry);
        releaseAdmission(entry);
        publish(entry, GpuPresentationTargetState::Retired, true,
                render::GpuPresentationTargetCode::Retired,
                entry.retirePending ? "attach retired before native creation"
                                    : "attach cancelled during shutdown");
        return;
    }
    if (quarantineFuseLatched()) {
        // A generation in this process could not be proven retired. Refuse to create any new native
        // target anywhere. No native state exists for this surface, so it is safe to destroy.
        entry.lastCode = render::GpuPresentationTargetCode::PresentationUnavailable;
        entry.message =
            "the process quarantine fuse has latched; no new native presentation targets";
        entry.state = GpuPresentationTargetState::Rejected;
        releaseOverlayCharge(entry);
        releaseSurfaceOwnership(entry);
        releaseAdmission(entry);
        publish(entry, GpuPresentationTargetState::Rejected, true, entry.lastCode, entry.message);
        return;
    }
    // Reserve the bounded quarantine slot BEFORE creating native state, so teardown never
    // allocates.
    QuarantineReservation reservation = kNoQuarantineReservation;
    if (!quarantineReserve(reservation)) {
        entry.lastCode = render::GpuPresentationTargetCode::PresentationUnavailable;
        entry.message = "the bounded process quarantine has no free slot; the attach is refused "
                        "before any native state is created";
        entry.state = GpuPresentationTargetState::Rejected;
        releaseOverlayCharge(entry);
        releaseSurfaceOwnership(entry);
        releaseAdmission(entry);
        publish(entry, GpuPresentationTargetState::Rejected, true, entry.lastCode, entry.message);
        return;
    }
    entry.reservation = reservation;
    auto created = render::GpuPresentationTarget::create(*device, entry.description);
    if (!created) {
        quarantineRelease(entry.reservation);
        entry.reservation = kNoQuarantineReservation;
        entry.lastCode = created.code;
        entry.message = created.message.empty() ? "the presentation target could not be created"
                                                : created.message;
        entry.state = GpuPresentationTargetState::Rejected;
        releaseOverlayCharge(entry);
        releaseSurfaceOwnership(entry);
        releaseAdmission(entry);
        // No native target exists, so no surface is referenced: safe for the UI to destroy.
        publish(entry, GpuPresentationTargetState::Rejected, true, created.code, entry.message);
        return;
    }
    entry.native = std::move(created.target);
    entry.info = entry.native->info();
    entry.state = GpuPresentationTargetState::Active;
    entry.lastCode = render::GpuPresentationTargetCode::Ok;
    publish(entry, GpuPresentationTargetState::Active, false, render::GpuPresentationTargetCode::Ok,
            {});
}

void CoordinatorState::drivePresent(Entry& entry) {
    const auto pollCode = entry.native->pollRetirement();
    if (pollCode == render::GpuPresentationTargetCode::DeviceLost) {
        quarantine(entry, "the device was lost during presentation");
        return;
    }

    if (entry.retirePending) {
        driveRetire(entry);
        return;
    }

    if (entry.pendingResize.has_value()) {
        const GpuPresentationPendingResize requested = *entry.pendingResize;
        render::GpuPresentationTargetDescription description = entry.description;
        description.width = requested.width;
        description.height = requested.height;
        const auto code = entry.native->recreate(description);
        if (code == render::GpuPresentationTargetCode::NotReady) {
            // Prior native work has not retired yet; retry on a later pump.
            return;
        }
        if (code != render::GpuPresentationTargetCode::Ok &&
            code != render::GpuPresentationTargetCode::Suboptimal) {
            quarantine(entry, entry.native->lastMessage().empty()
                                  ? "the swapchain could not be recreated"
                                  : entry.native->lastMessage());
            return;
        }
        entry.description = description;
        entry.info = entry.native->info();
        entry.pendingResize.reset();
        entry.lastCode = code;
        entry.state = GpuPresentationTargetState::Active;
        publish(entry, GpuPresentationTargetState::Active, false, code, {});
    }

    if (!entry.pendingUpdate.has_value() || entry.nativeAcquired) {
        return;
    }

    GpuPresentationPendingUpdate update = std::move(*entry.pendingUpdate);
    entry.pendingUpdate.reset();

    // Aggregate/per-overlay byte budgets are enforced at admission (see
    // GpuPresentationClient::update), so `update` is already within budget here.

    const auto acquireCode = entry.native->acquire();
    if (acquireCode == render::GpuPresentationTargetCode::NotReady) {
        // Keep the request and retry on the next pump; no busy loop.
        entry.pendingUpdate = std::move(update);
        return;
    }
    if (acquireCode == render::GpuPresentationTargetCode::OutOfDate) {
        entry.pendingUpdate = std::move(update);
        entry.pendingResize = GpuPresentationPendingResize{entry.info.width, entry.info.height, 0U};
        return;
    }
    if (acquireCode == render::GpuPresentationTargetCode::DeviceLost) {
        entry.pendingUpdate = std::move(update);
        quarantine(entry, "the device was lost while acquiring a swapchain image");
        return;
    }
    if (acquireCode != render::GpuPresentationTargetCode::Ok &&
        acquireCode != render::GpuPresentationTargetCode::Suboptimal) {
        entry.pendingUpdate = std::move(update);
        quarantine(entry, entry.native->lastMessage().empty()
                              ? "a swapchain image could not be acquired"
                              : entry.native->lastMessage());
        return;
    }
    entry.nativeAcquired = true;

    // A successful acquire proves the previous present/render is complete, so the previous pin (and
    // its borrowing alias) can now be released safely.
    releaseActiveImage(entry);
    if (entry.activeOverlayBytes != 0U) {
        std::lock_guard lock(mailbox->mutex);
        mailbox->chargedOverlayBytes -= entry.activeOverlayBytes;
        entry.activeOverlayBytes = 0;
    }

    auto pinned = registry->pin(update.lease);
    if (!pinned) {
        // The lease is stale/foreign. Consume the acquired image legally and keep the target
        // healthy; release the overlay charge for the dropped update.
        const auto discard = entry.native->present(render::GpuClearColor{});
        static_cast<void>(discard);
        entry.nativeAcquired = false;
        releaseOverlayCharge(entry);
        entry.message = "the resident lease was rejected: " + pinned.diagnostic.message;
        entry.lastCode = render::GpuPresentationTargetCode::InvalidArgument;
        publish(entry, entry.state, false, entry.lastCode, entry.message);
        return;
    }
    // The alias owns the pin through its deleter, so the presenter's retained input keeps the
    // resident image (and its registry byte charge) alive even after this entry is gone.
    entry.activeAlias =
        retainingAlias(std::make_shared<GpuResidentFramePin>(std::move(pinned.pin)));

    render::GpuPresentImageParams params = update.params;
    params.targetWidth = entry.info.width;
    params.targetHeight = entry.info.height;
    const auto presentCode =
        entry.native->presentImage(entry.activeAlias, params, overlayDescriptor(update.overlay));
    if (presentCode == render::GpuPresentationTargetCode::Ok ||
        presentCode == render::GpuPresentationTargetCode::Suboptimal) {
        entry.nativeAcquired = false;
        entry.appliedSequence = update.sequence;
        ++entry.presentCount;
        // The extracted pending charge becomes the applied active charge; the total is unchanged.
        entry.activeOverlayBytes = entry.chargedPendingBytes;
        entry.chargedPendingBytes = 0;
        entry.lastCode = presentCode;
        entry.state = GpuPresentationTargetState::Active;
        entry.message.clear();
        publish(entry, GpuPresentationTargetState::Active, false, presentCode, {});
        if (presentCode == render::GpuPresentationTargetCode::Suboptimal) {
            entry.pendingResize =
                GpuPresentationPendingResize{entry.info.width, entry.info.height, 0U};
        }
        return;
    }

    // Hard failure after a successful acquire. Discard the acquired image legally, then quarantine
    // the native target (retain it) rather than faking a safe acknowledgement.
    const auto discard = entry.native->present(render::GpuClearColor{});
    entry.nativeAcquired = false;
    const bool lost = discard == render::GpuPresentationTargetCode::DeviceLost;
    const std::string failure = entry.native->lastMessage().empty() ? "the resident present failed"
                                                                    : entry.native->lastMessage();
    quarantine(entry, lost ? "the device was lost while discarding a failed present" : failure);
}

void CoordinatorState::driveRetire(Entry& entry) {
    if (entry.nativeAcquired) {
        // Legally consume the acquired-unpresented image with a clear discard before retiring.
        const auto discard = entry.native->present(render::GpuClearColor{});
        entry.nativeAcquired = false;
        if (discard == render::GpuPresentationTargetCode::DeviceLost) {
            quarantine(entry, "the device was lost while discarding an acquired image");
            return;
        }
        if (discard != render::GpuPresentationTargetCode::Ok &&
            discard != render::GpuPresentationTargetCode::Suboptimal) {
            quarantine(entry, "the acquired image could not be legally discarded");
            return;
        }
    }
    entry.pendingUpdate.reset();
    entry.pendingResize.reset();
    const auto code = entry.native->beginRetire();
    if (code == render::GpuPresentationTargetCode::Retired) {
        // Proven by the presentation engine; the native target may now be destroyed on this owner
        // thread, and only then is Retired published to the UI.
        entry.native.reset();
        releaseActiveImage(entry);
        releaseOverlayCharge(entry);
        if (entry.reservation != kNoQuarantineReservation) {
            quarantineRelease(entry.reservation);
            entry.reservation = kNoQuarantineReservation;
        }
        entry.state = GpuPresentationTargetState::Retired;
        releaseSurfaceOwnership(entry);
        releaseAdmission(entry);
        publish(entry, GpuPresentationTargetState::Retired, true,
                render::GpuPresentationTargetCode::Retired, {});
        return;
    }
    if (code == render::GpuPresentationTargetCode::DeviceLost) {
        quarantine(entry, "the device was lost while retiring");
        return;
    }
    entry.state = GpuPresentationTargetState::Retiring;
    publish(entry, GpuPresentationTargetState::Retiring, false, code, entry.native->lastMessage());
}

void CoordinatorState::driveTarget(Entry& entry) {
    switch (entry.state) {
    case GpuPresentationTargetState::Attaching:
        driveAttach(entry);
        if (entry.state == GpuPresentationTargetState::Active ||
            entry.state == GpuPresentationTargetState::Retired) {
            if (entry.state == GpuPresentationTargetState::Active) {
                drivePresent(entry);
            }
        }
        return;
    case GpuPresentationTargetState::Active:
    case GpuPresentationTargetState::Resizing:
        drivePresent(entry);
        return;
    case GpuPresentationTargetState::Retiring:
        driveRetire(entry);
        return;
    case GpuPresentationTargetState::Retired:
    case GpuPresentationTargetState::Rejected:
    case GpuPresentationTargetState::Quarantined:
    case GpuPresentationTargetState::Unproven:
    case GpuPresentationTargetState::Gone:
        return;
    }
}

GpuPresentationShutdownStatus CoordinatorState::computeShutdownStatus() const {
    GpuPresentationShutdownStatus status;
    {
        std::lock_guard lock(mailbox->mutex);
        status.accepting = mailbox->accepting && !mailbox->ownerGone;
    }
    for (const auto& [id, entry] : entries) {
        static_cast<void>(id);
        switch (entry.state) {
        case GpuPresentationTargetState::Retired:
        case GpuPresentationTargetState::Rejected:
            ++status.retiredTargets;
            break;
        case GpuPresentationTargetState::Quarantined:
            ++status.quarantinedTargets;
            break;
        case GpuPresentationTargetState::Unproven:
        case GpuPresentationTargetState::Attaching:
        case GpuPresentationTargetState::Active:
        case GpuPresentationTargetState::Resizing:
        case GpuPresentationTargetState::Retiring:
        case GpuPresentationTargetState::Gone:
            ++status.unprovenTargets;
            break;
        }
    }
    status.activeTargets = status.quarantinedTargets + status.unprovenTargets;
    status.drained = status.activeTargets == 0U;
    status.message = status.drained
                         ? "all presentation targets reached a terminal state"
                         : "presentation targets are retained pending device-generation teardown";
    return status;
}

void CoordinatorState::publishShutdownSnapshot() {
    if (!shutdownStatusDirty) {
        // No owner state change since the last publication can alter the summary. Rewriting it
        // would take the mailbox lock and reassign the same message on every idle pump; skip it.
        return;
    }
    lastShutdownStatus = computeShutdownStatus();
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->shutdownSnapshot = lastShutdownStatus;
        mailbox->shutdownSnapshotValid = true;
    }
    shutdownStatusDirty = false;
}

bool CoordinatorState::hasPendingWork() const {
    // Owner thread only. A target still needs the owner to drive it while it is attaching,
    // resizing, or retiring, or while it has an extracted pending update/resize/retire or an
    // acquired image awaiting present. A stable Active target with nothing extracted is idle: the
    // client's wake hook re-arms the loop the instant a new request arrives, so the loop can wait
    // instead of republishing the same snapshot thousands of times a second.
    for (const auto& [id, entry] : entries) {
        static_cast<void>(id);
        switch (entry.state) {
        case GpuPresentationTargetState::Attaching:
        case GpuPresentationTargetState::Resizing:
        case GpuPresentationTargetState::Retiring:
            return true;
        case GpuPresentationTargetState::Active:
            if (entry.pendingUpdate.has_value() || entry.pendingResize.has_value() ||
                entry.retirePending || entry.nativeAcquired) {
                return true;
            }
            break;
        case GpuPresentationTargetState::Retired:
        case GpuPresentationTargetState::Rejected:
        case GpuPresentationTargetState::Quarantined:
        case GpuPresentationTargetState::Unproven:
        case GpuPresentationTargetState::Gone:
            break;
        }
    }
    // Un-ingested client requests. The client wake hook normally restarts the loop immediately;
    // this covers a request that landed between the pump's drain and the wait-decision without a
    // lost wakeup.
    std::lock_guard lock(mailbox->mutex);
    for (const auto& [id, slot] : mailbox->slots) {
        static_cast<void>(id);
        if (slot.attachPending || slot.update.has_value() || slot.resize.has_value() ||
            slot.retirePending) {
            return true;
        }
    }
    return !mailbox->forgetRequested.empty();
}

void CoordinatorState::pumpOnce() {
    ingestRequests();
    if (shuttingDown) {
        ++shutdownPumps;
    }
    // Drive a snapshot of ids; entries may be marked terminal but are never erased so status
    // remains queryable and admission is only released once.
    std::vector<GpuPresentationTargetId> ids;
    ids.reserve(entries.size());
    for (const auto& [id, entry] : entries) {
        static_cast<void>(entry);
        ids.push_back(id);
    }
    for (const GpuPresentationTargetId id : ids) {
        const auto it = entries.find(id);
        if (it == entries.end()) {
            continue;
        }
        driveTarget(it->second);
    }
    if (shuttingDown && shutdownPumps >= options.shutdownDrainPumps) {
        for (auto& [id, entry] : entries) {
            static_cast<void>(id);
            if (!terminalState(entry.state)) {
                entry.state = GpuPresentationTargetState::Unproven;
                releaseOverlayCharge(entry);
                releaseAdmission(entry);
                publish(entry, GpuPresentationTargetState::Unproven, false,
                        render::GpuPresentationTargetCode::RetirePending,
                        "shutdown drain budget exhausted; the native target is retained and the "
                        "surface must stay alive");
            }
        }
    }
    // Publish the owner summary for synchronized off-owner reads without entry traversal.
    publishShutdownSnapshot();
}

} // namespace bloom::runtime::presentation_coordinator_detail
