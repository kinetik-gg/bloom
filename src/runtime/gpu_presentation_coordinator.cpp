// Public surface and owner-side construction/shutdown for the presentation coordinator. The drive
// state machine and bounded quarantine live in gpu_presentation_coordinator_pump.cpp; this file
// maps the public class onto that state and exposes client creation, wake hookup, pump, and
// shutdown.

#include "gpu_presentation_coordinator_state.hpp"

#include <bloom/render/gpu_device.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace bloom::runtime {

using presentation_coordinator_detail::CoordinatorState;

struct GpuPresentationCoordinator::Impl final : CoordinatorState {
    using CoordinatorState::CoordinatorState;
};

GpuPresentationCoordinator::GpuPresentationCoordinator(render::GpuDevice& device,
                                                       GpuResidentFrameLeaseRegistry& registry,
                                                       GpuPresentationCoordinatorOptions options)
    : impl_(std::make_unique<Impl>(&device, &registry, options)) {
    impl_->ownerThread = std::this_thread::get_id();
    // Reserve the process quarantine storage now (non-teardown path) so teardown never allocates.
    impl_->valid = device.isOwnerThread() &&
                   presentation_coordinator_detail::quarantineEnsureStorage() &&
                   !presentation_coordinator_detail::quarantineFuseLatched();
    impl_->mailbox = std::make_shared<detail::GpuPresentationMailbox>();
    {
        std::lock_guard lock(impl_->mailbox->mutex);
        impl_->mailbox->maxTargets = options.maxTargets;
        impl_->mailbox->maxRetainedRecords = options.maxRetainedTargets;
        impl_->mailbox->maxOverlayBytes = options.maxOverlayBytes;
        impl_->mailbox->maxOverlayBytesPerFrame = options.maxOverlayBytesPerFrame;
        impl_->mailbox->maxTargetExtent = options.maxTargetExtent;
        impl_->mailbox->maxOverlayExtent = options.maxOverlayExtent;
        if (impl_->valid) {
            const auto view = device.borrowedInstanceView();
            if (view.valid) {
                impl_->mailbox->view = view;
            }
        }
    }
    impl_->client =
        std::shared_ptr<GpuPresentationClient>(new GpuPresentationClient(impl_->mailbox));
}

GpuPresentationCoordinator::~GpuPresentationCoordinator() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard lock(impl_->mailbox->mutex);
        impl_->mailbox->ownerGone = true;
        impl_->mailbox->accepting = false;
    }
    for (auto& [id, entry] : impl_->entries) {
        static_cast<void>(id);
        if (entry.native != nullptr) {
            // Never destroy a target that references a surface the presentation engine may still
            // read: consume the slot reserved when it was admitted and move the whole generation
            // (native target + retaining lease alias) into the bounded, non-destructed process
            // quarantine. The quota was reserved before creation, so this cannot fail or allocate.
            if (entry.reservation != presentation_coordinator_detail::kNoQuarantineReservation) {
                presentation_coordinator_detail::quarantineCommit(
                    entry.reservation, std::move(entry.native), std::move(entry.activeAlias));
                entry.reservation = presentation_coordinator_detail::kNoQuarantineReservation;
            } else {
                // Defensive: latch the fuse and leak the raw generation rather than destroy it.
                presentation_coordinator_detail::latchQuarantineFuse();
                static_cast<void>(entry.native.release()); // NOLINT(bugprone-unused-return-value)
            }
        } else {
            impl_->releaseActiveImage(entry);
            if (entry.reservation != presentation_coordinator_detail::kNoQuarantineReservation) {
                presentation_coordinator_detail::quarantineRelease(entry.reservation);
                entry.reservation = presentation_coordinator_detail::kNoQuarantineReservation;
            }
        }
    }
    impl_->entries.clear();
}

std::shared_ptr<GpuPresentationClient> GpuPresentationCoordinator::client() const {
    return impl_ != nullptr ? impl_->client : nullptr;
}

void GpuPresentationCoordinator::setWakeCallback(std::function<void()> callback) {
    if (impl_ == nullptr || impl_->mailbox == nullptr) {
        return;
    }
    std::lock_guard lock(impl_->mailbox->mutex);
    impl_->mailbox->wake = std::move(callback);
}

void GpuPresentationCoordinator::pump() {
    if (impl_ == nullptr || !impl_->valid || !impl_->ownerThreadHere()) {
        return;
    }
    impl_->pumpOnce();
}

void GpuPresentationCoordinator::beginShutdown() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->shuttingDown = true;
    impl_->shutdownPumps = 0;
    impl_->shutdownStatusDirty = true;
    std::lock_guard lock(impl_->mailbox->mutex);
    impl_->mailbox->accepting = false;
    // Drop pending updates/resizes so shutdown does not perform fresh present work.
    for (auto& [id, slot] : impl_->mailbox->slots) {
        static_cast<void>(id);
        if (slot.update.has_value()) {
            impl_->mailbox->chargedOverlayBytes -= slot.pendingOverlayBytes;
        }
        slot.pendingOverlayBytes = 0;
        slot.update.reset();
        slot.resize.reset();
        slot.retirePending = true;
    }
}

GpuPresentationShutdownStatus GpuPresentationCoordinator::shutdownStatus() const {
    GpuPresentationShutdownStatus status;
    if (impl_ == nullptr) {
        status.drained = true;
        status.message = "the presentation coordinator has no owner";
        return status;
    }
    if (impl_->ownerThreadHere()) {
        impl_->publishShutdownSnapshot();
        return impl_->lastShutdownStatus;
    }
    // Off the owner thread: return the last owner-published snapshot. A UI caller never traverses
    // owner entries, which are only safe to read on the owner thread.
    std::lock_guard lock(impl_->mailbox->mutex);
    if (impl_->mailbox->shutdownSnapshotValid) {
        return impl_->mailbox->shutdownSnapshot;
    }
    status.accepting = false;
    status.message = "no owner pump has published a shutdown summary yet";
    return status;
}

bool GpuPresentationCoordinator::isShuttingDown() const noexcept {
    return impl_ != nullptr && impl_->shuttingDown;
}

bool GpuPresentationCoordinator::hasPendingWork() const {
    // Owner-thread diagnostic. Off the owner thread, or before construction completed, it reports
    // false; a request always enqueues a wake through the client, so the driver cannot miss it.
    return impl_ != nullptr && impl_->valid && impl_->ownerThreadHere() && impl_->hasPendingWork();
}

std::size_t GpuPresentationCoordinator::quarantinedGenerationCount() noexcept {
    return presentation_coordinator_detail::quarantinedCount();
}

bool GpuPresentationCoordinator::quarantineCapacityReached() noexcept {
    return presentation_coordinator_detail::quarantineCapReached();
}

} // namespace bloom::runtime
