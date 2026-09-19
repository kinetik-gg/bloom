// UI-side port implementation: immutable overlay storage, the typed client, and the shared-mailbox
// transport. It contains no device, no Vulkan, and no owner pointer; every native check happens in
// gpu_presentation_coordinator.cpp on the owner thread.

#include "gpu_presentation_coordinator_private.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

using detail::GpuPresentationClientSlot;
using detail::GpuPresentationMailbox;
using detail::GpuPresentationPendingResize;
using detail::GpuPresentationPendingUpdate;

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

void invokeWake(const std::shared_ptr<detail::GpuPresentationMailbox>& mailbox) noexcept {
    std::function<void()> wake;
    {
        std::lock_guard lock(mailbox->mutex);
        wake = mailbox->wake;
    }
    if (wake) {
        wake();
    }
}

} // namespace

// --- GpuPresentationOverlay ---------------------------------------------------------------------

GpuPresentationOverlay::GpuPresentationOverlay(std::vector<std::uint8_t> pixels,
                                               const std::uint32_t width,
                                               const std::uint32_t height,
                                               const std::uint32_t rowStrideBytes,
                                               const std::uint64_t token) noexcept
    : pixels_(std::move(pixels)), token_(token), width_(width), height_(height),
      rowStrideBytes_(rowStrideBytes) {
    byteSize_ = static_cast<std::uint64_t>(rowStrideBytes_) * static_cast<std::uint64_t>(height_);
}

std::shared_ptr<const GpuPresentationOverlay>
GpuPresentationOverlay::create(std::vector<std::uint8_t> pixels, const std::uint32_t width,
                               const std::uint32_t height, const std::uint32_t rowStrideBytes,
                               const std::uint64_t token, const std::uint64_t byteBudget) noexcept {
    if (width == 0U || height == 0U || token == 0U) {
        return nullptr;
    }
    const std::uint64_t minimumStride = static_cast<std::uint64_t>(width) * 4ULL;
    const std::uint64_t stride =
        rowStrideBytes != 0U ? static_cast<std::uint64_t>(rowStrideBytes) : minimumStride;
    if (stride < minimumStride) {
        return nullptr;
    }
    const std::uint64_t rowBytes64 = static_cast<std::uint64_t>(height) * stride;
    if (rowBytes64 > byteBudget || rowBytes64 > static_cast<std::uint64_t>(pixels.size())) {
        return nullptr;
    }
    if (stride > static_cast<std::uint64_t>(UINT32_MAX)) {
        return nullptr;
    }
    try {
        return std::shared_ptr<const GpuPresentationOverlay>(new GpuPresentationOverlay(
            std::move(pixels), width, height, static_cast<std::uint32_t>(stride), token));
    } catch (const std::bad_alloc&) {
        // Fail closed: a malformed overlay already reports null, so an allocation failure must not
        // escape this noexcept factory. The caller then refuses the update.
        return nullptr;
    }
}

// --- GpuPresentationClient ----------------------------------------------------------------------

GpuPresentationClient::GpuPresentationClient() noexcept = default;
GpuPresentationClient::~GpuPresentationClient() = default;
GpuPresentationClient::GpuPresentationClient(GpuPresentationClient&&) noexcept = default;
GpuPresentationClient& GpuPresentationClient::operator=(GpuPresentationClient&&) noexcept = default;

GpuPresentationClient::GpuPresentationClient(
    std::shared_ptr<detail::GpuPresentationMailbox> mailbox) noexcept
    : mailbox_(std::move(mailbox)) {}

render::GpuBorrowedInstanceView GpuPresentationClient::instanceView() const {
    if (mailbox_ == nullptr) {
        return {};
    }
    std::lock_guard lock(mailbox_->mutex);
    if (mailbox_->ownerGone) {
        return {};
    }
    return mailbox_->view;
}

GpuPresentationPortResult GpuPresentationClient::attach(const render::GpuBorrowedSurface& surface,
                                                        const std::uint32_t width,
                                                        const std::uint32_t height) {
    GpuPresentationPortResult result;
    result.sequence = 0;
    if (mailbox_ == nullptr) {
        result.code = GpuPresentationPortCode::OwnerGone;
        return result;
    }
    if (surface.surface_bits == 0U || width == 0U || height == 0U) {
        result.code = GpuPresentationPortCode::Rejected;
        result.message = "attach requires a surface and a positive extent";
        return result;
    }
    {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->ownerGone) {
            result.code = GpuPresentationPortCode::OwnerGone;
            return result;
        }
        if (!mailbox_->accepting) {
            result.code = GpuPresentationPortCode::ShuttingDown;
            return result;
        }
        // A latched process fuse stops new native targets on every existing coordinator, not only
        // freshly constructed ones. No native state is created for a refused attach.
        if (presentation_coordinator_detail::quarantineFuseLatched()) {
            result.code = GpuPresentationPortCode::QuarantineFused;
            result.message = "the process quarantine fuse has latched; no new native targets";
            return result;
        }
        // Bounded retained records: without an adapter calling forget(), proven-terminal records
        // stay visible to status(); refuse new attaches rather than grow entries/snapshots without
        // bound.
        if (mailbox_->snapshots.size() >= mailbox_->maxRetainedRecords) {
            result.code = GpuPresentationPortCode::TooManyTargets;
            result.message =
                "the bounded retained-record cap is reached; forget() a retired target";
            return result;
        }
        // A borrowed surface (device epoch + bits) may be owned by exactly one live target. A
        // second attach over the same surface must not create a second non-retired swapchain, and
        // it must NOT disturb the existing owner (so the caller may not treat this rejection as
        // safe-to-destroy). This is checked before the cap so a duplicate is reported precisely and
        // the existing owner is never disturbed by a cap refusal.
        const detail::GpuPresentationMailbox::SurfaceKey key{surface.epoch.value,
                                                             surface.surface_bits};
        if (mailbox_->ownedSurfaces.find(key) != mailbox_->ownedSurfaces.end()) {
            result.code = GpuPresentationPortCode::DuplicateSurface;
            result.message = "the borrowed surface is already owned by a live presentation target";
            return result;
        }
        if (mailbox_->admittedTargets >= mailbox_->maxTargets) {
            result.code = GpuPresentationPortCode::TooManyTargets;
            result.message = "the presentation target cap is full";
            return result;
        }
        const GpuPresentationTargetId id = mailbox_->nextTargetId.fetch_add(1U);
        GpuPresentationClientSlot slot;
        slot.attachPending = true;
        slot.surface = surface;
        slot.attachWidth = width;
        slot.attachHeight = height;
        mailbox_->slots[id] = std::move(slot);
        GpuPresentationTargetSnapshot snapshot;
        snapshot.id = id;
        snapshot.state = GpuPresentationTargetState::Attaching;
        mailbox_->snapshots[id] = std::move(snapshot);
        mailbox_->ownedSurfaces[key] = id;
        ++mailbox_->admittedTargets;
        result.code = GpuPresentationPortCode::Accepted;
        result.target = id;
    }
    invokeWake(mailbox_);
    return result;
}

GpuPresentationPortResult GpuPresentationClient::update(const GpuPresentationTargetId target,
                                                        const std::uint64_t sequence,
                                                        GpuPresentationUpdate updateValue) {
    GpuPresentationPortResult result;
    result.target = target;
    result.sequence = sequence;
    if (mailbox_ == nullptr) {
        result.code = GpuPresentationPortCode::OwnerGone;
        return result;
    }
    {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->ownerGone) {
            result.code = GpuPresentationPortCode::OwnerGone;
            return result;
        }
        if (!mailbox_->accepting) {
            result.code = GpuPresentationPortCode::ShuttingDown;
            return result;
        }
        const auto slotIt = mailbox_->slots.find(target);
        if (slotIt == mailbox_->slots.end()) {
            result.code = GpuPresentationPortCode::UnknownTarget;
            return result;
        }
        const auto snapshotIt = mailbox_->snapshots.find(target);
        if (snapshotIt != mailbox_->snapshots.end() &&
            (terminalState(snapshotIt->second.state) ||
             snapshotIt->second.state == GpuPresentationTargetState::Retiring)) {
            result.code = GpuPresentationPortCode::Closed;
            result.message = "the target no longer admits updates";
            return result;
        }
        GpuPresentationClientSlot& slot = slotIt->second;
        if (slot.admissionClosed) {
            result.code = GpuPresentationPortCode::Closed;
            result.message = "the target no longer admits updates";
            return result;
        }
        if (sequence == 0U || sequence <= slot.highestSequence) {
            result.code = GpuPresentationPortCode::StaleSequence;
            result.message = "a newer request was already accepted for this target";
            return result;
        }
        if (!updateValue.lease.isValid()) {
            result.code = GpuPresentationPortCode::Rejected;
            result.message = "the update carries no valid resident lease";
            return result;
        }
        const std::uint64_t newBytes =
            updateValue.overlay != nullptr ? updateValue.overlay->byteSize() : 0U;
        if (newBytes > mailbox_->maxOverlayBytesPerFrame) {
            result.code = GpuPresentationPortCode::OverBudget;
            result.message = "the overlay exceeds the per-frame byte budget";
            return result;
        }
        const std::uint64_t base = mailbox_->chargedOverlayBytes - slot.pendingOverlayBytes;
        if (base + newBytes > mailbox_->maxOverlayBytes) {
            result.code = GpuPresentationPortCode::OverBudget;
            result.message = "the aggregate overlay byte budget is exhausted";
            return result;
        }
        const bool coalesced = slot.update.has_value();
        mailbox_->chargedOverlayBytes = base + newBytes;
        slot.pendingOverlayBytes = newBytes;
        slot.highestSequence = sequence;
        GpuPresentationPendingUpdate pending;
        pending.lease = updateValue.lease;
        pending.params = updateValue.params;
        pending.overlay = std::move(updateValue.overlay);
        pending.sequence = sequence;
        pending.overlayBytes = newBytes;
        slot.update = std::move(pending);
        result.code =
            coalesced ? GpuPresentationPortCode::Coalesced : GpuPresentationPortCode::Accepted;
    }
    invokeWake(mailbox_);
    return result;
}

GpuPresentationPortResult GpuPresentationClient::resize(const GpuPresentationTargetId target,
                                                        const std::uint64_t sequence,
                                                        const std::uint32_t width,
                                                        const std::uint32_t height) {
    GpuPresentationPortResult result;
    result.target = target;
    result.sequence = sequence;
    if (mailbox_ == nullptr) {
        result.code = GpuPresentationPortCode::OwnerGone;
        return result;
    }
    if (width == 0U || height == 0U) {
        result.code = GpuPresentationPortCode::Rejected;
        result.message = "resize requires a positive extent";
        return result;
    }
    {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->ownerGone) {
            result.code = GpuPresentationPortCode::OwnerGone;
            return result;
        }
        if (!mailbox_->accepting) {
            result.code = GpuPresentationPortCode::ShuttingDown;
            return result;
        }
        if (width > mailbox_->maxTargetExtent || height > mailbox_->maxTargetExtent) {
            result.code = GpuPresentationPortCode::Rejected;
            result.message = "the requested extent exceeds the maximum target extent";
            return result;
        }
        const auto slotIt = mailbox_->slots.find(target);
        if (slotIt == mailbox_->slots.end()) {
            result.code = GpuPresentationPortCode::UnknownTarget;
            return result;
        }
        const auto snapshotIt = mailbox_->snapshots.find(target);
        if (snapshotIt != mailbox_->snapshots.end() &&
            (terminalState(snapshotIt->second.state) ||
             snapshotIt->second.state == GpuPresentationTargetState::Retiring)) {
            result.code = GpuPresentationPortCode::Closed;
            return result;
        }
        GpuPresentationClientSlot& slot = slotIt->second;
        if (slot.admissionClosed) {
            result.code = GpuPresentationPortCode::Closed;
            return result;
        }
        if (sequence == 0U || sequence <= slot.highestSequence) {
            result.code = GpuPresentationPortCode::StaleSequence;
            return result;
        }
        const bool coalesced = slot.resize.has_value();
        slot.highestSequence = sequence;
        GpuPresentationPendingResize pending;
        pending.width = width;
        pending.height = height;
        pending.sequence = sequence;
        slot.resize = pending;
        result.code =
            coalesced ? GpuPresentationPortCode::Coalesced : GpuPresentationPortCode::Accepted;
    }
    invokeWake(mailbox_);
    return result;
}

GpuPresentationPortResult GpuPresentationClient::retire(const GpuPresentationTargetId target,
                                                        const std::uint64_t sequence) {
    GpuPresentationPortResult result;
    result.target = target;
    result.sequence = sequence;
    if (mailbox_ == nullptr) {
        result.code = GpuPresentationPortCode::OwnerGone;
        return result;
    }
    {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->ownerGone) {
            result.code = GpuPresentationPortCode::OwnerGone;
            return result;
        }
        const auto slotIt = mailbox_->slots.find(target);
        if (slotIt == mailbox_->slots.end()) {
            result.code = GpuPresentationPortCode::UnknownTarget;
            return result;
        }
        const auto snapshotIt = mailbox_->snapshots.find(target);
        if (snapshotIt != mailbox_->snapshots.end() && terminalState(snapshotIt->second.state)) {
            result.code = GpuPresentationPortCode::Closed;
            return result;
        }
        GpuPresentationClientSlot& slot = slotIt->second;
        if (slot.admissionClosed) {
            result.code = GpuPresentationPortCode::Closed;
            return result;
        }
        // Retire drains any unapplied request and releases its overlay charge.
        if (slot.update.has_value()) {
            mailbox_->chargedOverlayBytes -= slot.pendingOverlayBytes;
        }
        slot.pendingOverlayBytes = 0;
        slot.update.reset();
        slot.resize.reset();
        slot.admissionClosed = true;
        slot.retirePending = true;
        slot.retireSequence = sequence;
        slot.highestSequence = std::max(slot.highestSequence, sequence);
        result.code = GpuPresentationPortCode::Accepted;
    }
    invokeWake(mailbox_);
    return result;
}

GpuPresentationPortResult GpuPresentationClient::cancel(const GpuPresentationTargetId target,
                                                        const std::uint64_t sequence) {
    GpuPresentationPortResult result;
    result.target = target;
    result.sequence = sequence;
    if (mailbox_ == nullptr) {
        result.code = GpuPresentationPortCode::OwnerGone;
        return result;
    }
    {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->ownerGone) {
            result.code = GpuPresentationPortCode::OwnerGone;
            return result;
        }
        const auto slotIt = mailbox_->slots.find(target);
        if (slotIt == mailbox_->slots.end()) {
            result.code = GpuPresentationPortCode::UnknownTarget;
            return result;
        }
        GpuPresentationClientSlot& slot = slotIt->second;
        if (!slot.update.has_value() || (sequence != 0U && slot.update->sequence != sequence)) {
            result.code = GpuPresentationPortCode::StaleSequence;
            result.message = "no matching unapplied update to cancel";
            return result;
        }
        mailbox_->chargedOverlayBytes -= slot.pendingOverlayBytes;
        slot.pendingOverlayBytes = 0;
        slot.update.reset();
        result.code = GpuPresentationPortCode::Accepted;
    }
    invokeWake(mailbox_);
    return result;
}

GpuPresentationPortResult GpuPresentationClient::forget(const GpuPresentationTargetId target) {
    GpuPresentationPortResult result;
    result.target = target;
    if (mailbox_ == nullptr) {
        result.code = GpuPresentationPortCode::OwnerGone;
        return result;
    }
    {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->ownerGone) {
            result.code = GpuPresentationPortCode::OwnerGone;
            return result;
        }
        const auto snapshotIt = mailbox_->snapshots.find(target);
        if (snapshotIt == mailbox_->snapshots.end()) {
            result.code = GpuPresentationPortCode::UnknownTarget;
            result.message = "no record is retained for the target";
            return result;
        }
        const GpuPresentationTargetState state = snapshotIt->second.state;
        const bool terminalSafe = (state == GpuPresentationTargetState::Retired ||
                                   state == GpuPresentationTargetState::Rejected) &&
                                  snapshotIt->second.surfaceSafeToDestroy;
        if (!terminalSafe) {
            // Live or unproven: its surface ownership must stay retained. Never forget it.
            result.code = GpuPresentationPortCode::NotTerminal;
            result.message =
                "the target is not proven terminal; its record and surface are retained";
            return result;
        }
        // Drop the UI-visible record now and ask the owner to erase its entry on the next pump.
        mailbox_->snapshots.erase(snapshotIt);
        mailbox_->forgetRequested.insert(target);
        result.code = GpuPresentationPortCode::Accepted;
    }
    invokeWake(mailbox_);
    return result;
}

GpuPresentationTargetSnapshot
GpuPresentationClient::status(const GpuPresentationTargetId target) const {
    GpuPresentationTargetSnapshot snapshot;
    snapshot.id = target;
    if (mailbox_ == nullptr) {
        return snapshot;
    }
    std::lock_guard lock(mailbox_->mutex);
    if (mailbox_->ownerGone) {
        snapshot.state = GpuPresentationTargetState::Gone;
        snapshot.message = "the presentation owner has been torn down";
        return snapshot;
    }
    const auto it = mailbox_->snapshots.find(target);
    if (it == mailbox_->snapshots.end()) {
        snapshot.state = GpuPresentationTargetState::Gone;
        return snapshot;
    }
    return it->second;
}

bool GpuPresentationClient::ownerAlive() const noexcept {
    if (mailbox_ == nullptr) {
        return false;
    }
    std::lock_guard lock(mailbox_->mutex);
    return !mailbox_->ownerGone;
}

// Test-only factory used by the CPU-only port validation test.
std::shared_ptr<GpuPresentationClient>
GpuPresentationClient::createForTesting(std::shared_ptr<detail::GpuPresentationMailbox> mailbox) {
    return std::shared_ptr<GpuPresentationClient>(new GpuPresentationClient(std::move(mailbox)));
}

} // namespace bloom::runtime
