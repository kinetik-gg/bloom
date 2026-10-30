#include <bloom/render/native_image_storage.hpp>

#include "gpu_processing_phase_budget_private.hpp"

#include <utility>

namespace bloom::render {
using detail::incrementSaturating;
using detail::remaining;

NativeImageStorageToken::NativeImageStorageToken(std::shared_ptr<GpuProcessingPhaseLease> phase,
                                                 const std::uint64_t bytes,
                                                 const NativeImageStorageIdentity identity) noexcept
    : state_(phase->state_), origin_(std::move(phase)), originId_(origin_->id_), bytes_(bytes),
      identity_(identity) {}
NativeImageStorageToken::NativeImageStorageToken(GpuProcessingRootIdentity root,
                                                 const std::uint64_t bytes,
                                                 NativeImageStorageIdentity identity) noexcept
    : state_(std::const_pointer_cast<detail::GpuProcessingPhaseBudgetState>(std::move(root))),
      bytes_(bytes), identity_(identity), presentation_(true) {}
NativeImageStorageToken::NativeImageStorageToken(NativeImageStorageToken&& other) noexcept
    : state_(std::move(other.state_)), origin_(std::move(other.origin_)),
      publishedOrigin_(std::move(other.publishedOrigin_)),
      originId_(std::exchange(other.originId_, 0)), bytes_(std::exchange(other.bytes_, 0)),
      identity_(other.identity_), published_(std::exchange(other.published_, false)),
      presentation_(std::exchange(other.presentation_, false)) {}
NativeImageStorageToken&
NativeImageStorageToken::operator=(NativeImageStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        origin_ = std::move(other.origin_);
        publishedOrigin_ = std::move(other.publishedOrigin_);
        originId_ = std::exchange(other.originId_, 0);
        bytes_ = std::exchange(other.bytes_, 0);
        identity_ = other.identity_;
        published_ = std::exchange(other.published_, false);
        presentation_ = std::exchange(other.presentation_, false);
    }
    return *this;
}
NativeImageStorageToken::~NativeImageStorageToken() { reset(); }

NativeImageStorageGrant NativeImageStorageToken::reservePresentation(
    const GpuProcessingRootIdentity& root, const std::uint64_t bytes,
    const NativeImageStorageIdentity identity,
    const std::chrono::steady_clock::time_point now) noexcept {
    if (!root || bytes == 0 || identity.allocatorGeneration == 0)
        return {};
    auto state = std::const_pointer_cast<detail::GpuProcessingPhaseBudgetState>(root);
    const auto admission =
        detail::admitPresentation(*state, bytes, identity.deviceLocal, true, now);
    if (admission.status != GpuProcessingPhaseAdmission::Accepted)
        return {.status = admission.status,
                .domain = admission.domain,
                .token = {},
                .revisionAfterOwnRelease = admission.revision};
    return {.status = admission.status,
            .domain = admission.domain,
            .token = NativeImageStorageToken(root, bytes, identity)};
}

NativeImageStorageGrant
NativeImageStorageToken::reserve(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                 const std::uint64_t bytes,
                                 const NativeImageStorageIdentity identity,
                                 const std::chrono::steady_clock::time_point now) noexcept {
    if (!phase || bytes == 0 || identity.allocatorGeneration == 0)
        return {};
    const std::lock_guard lock(phase->state_->mutex);
    auto& state = *phase->state_;
    auto& counters = state.counters;
    if (phase->id_ == 0 || phase->deviceBytes_ == 0 || phase->unproven_)
        return {};
    const auto refuse = [&](const GpuProcessingPhaseAdmission status,
                            const GpuProcessingPhaseDomain domain) {
        if (status == GpuProcessingPhaseAdmission::BudgetRefused)
            incrementSaturating(counters.budgetRefusals);
        return NativeImageStorageGrant{.status = status,
                                       .domain = domain,
                                       .token = {},
                                       .revisionAfterOwnRelease =
                                           counters.admissionRevision ==
                                                   std::numeric_limits<std::uint64_t>::max()
                                               ? counters.admissionRevision
                                               : counters.admissionRevision + 1};
    };
    if (bytes > phase->remainingDeviceBytes_ ||
        (bytes > phase->deviceClaimBytes_ &&
         (!counters.deviceCapacityBytes ||
          bytes - phase->deviceClaimBytes_ >
              remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes))))
        return refuse(GpuProcessingPhaseAdmission::BudgetRefused, GpuProcessingPhaseDomain::Device);
    if (!identity.deviceLocal) {
        if (counters.hostReady && now > state.hostValidUntil) {
            counters.hostReady = false;
            incrementSaturating(counters.admissionRevision);
        }
        if (!counters.hostReady || !counters.hostCapacityBytes || now > state.hostValidUntil)
            return refuse(GpuProcessingPhaseAdmission::AwaitingCapacity,
                          GpuProcessingPhaseDomain::Host);
        if (bytes > remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes))
            return refuse(GpuProcessingPhaseAdmission::BudgetRefused,
                          GpuProcessingPhaseDomain::Host);
    }
    // Native ownership replaces admitted device debit; supplemental nonlocal host is NEW credit.
    // Checked subtraction above bounds both additions; no cross-heap half-transaction can escape.
    static_cast<void>(phase->drawDeviceLocked(bytes));
    counters.ownedNativeImageBytes += bytes;
    ++counters.ownedNativeImages;
    if (!identity.deviceLocal) {
        counters.ownedNonlocalNativeHostBytes += bytes;
        counters.totalHostClaimBytes += bytes;
    }
    return {.status = GpuProcessingPhaseAdmission::Accepted,
            .domain = GpuProcessingPhaseDomain::None,
            .token = NativeImageStorageToken(phase, bytes, identity)};
}

bool NativeImageStorageToken::reconcile(const std::uint64_t actual,
                                        const NativeImageStorageIdentity identity) noexcept {
    if (!state_ || actual == 0 || identity != identity_)
        return false;
    const std::lock_guard lock(state_->mutex);
    if (published_ || (!origin_ && !presentation_) || actual > bytes_)
        return false;
    const auto returned = bytes_ - actual;
    bytes_ = actual;
    auto& counters = state_->counters;
    if (presentation_) {
        counters.totalDeviceClaimBytes -= returned;
        counters.ownedPresentationImageBytes -= returned;
    } else {
        origin_->returnDeviceLocked(returned);
    }
    counters.ownedNativeImageBytes -= returned;
    if (!identity_.deviceLocal) {
        counters.ownedNonlocalNativeHostBytes -= returned;
        counters.totalHostClaimBytes -= returned;
    }
    return true;
}

bool NativeImageStorageToken::promote() noexcept {
    if (!state_)
        return false;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (presentation_) {
            published_ = true;
            return true;
        }
        if (published_)
            return true;
        if (!origin_ || origin_->unproven_)
            return false;
        published_ = true;
        publishedOrigin_ = origin_;
        releasedOrigin = std::move(origin_);
    }
    releasedOrigin.reset();
    return true;
}

bool NativeImageStorageToken::releaseFailedAttemptAfterBackingDestroyed(
    const std::shared_ptr<GpuProcessingPhaseLease>& phase) noexcept {
    if (!state_ || presentation_ || !phase || phase->state_ != state_ || phase->id_ != originId_)
        return false;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (phase->unproven_)
            return false;
        auto& counters = state_->counters;
        counters.ownedNativeImageBytes -= bytes_;
        --counters.ownedNativeImages;
        phase->returnDeviceLocked(bytes_);
        if (!identity_.deviceLocal) {
            counters.ownedNonlocalNativeHostBytes -= bytes_;
            counters.totalHostClaimBytes -= bytes_;
        }
        bytes_ = 0;
        originId_ = 0;
        published_ = false;
        releasedOrigin = std::move(origin_);
    }
    releasedOrigin.reset();
    publishedOrigin_.reset();
    state_.reset();
    return true;
}

void NativeImageStorageToken::reset() noexcept {
    if (!state_)
        return;
    // Acquire the weak origin before the neutral mutex, and keep/drop that temporary alias outside
    // it. The lease destructor takes this same mutex; published storage never holds it alive.
    auto liveOrigin = published_ ? publishedOrigin_.lock() : nullptr;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        auto& counters = state_->counters;
        counters.ownedNativeImageBytes -= bytes_;
        --counters.ownedNativeImages;
        if (presentation_) {
            counters.totalDeviceClaimBytes -= bytes_;
            counters.ownedPresentationImageBytes -= bytes_;
            --counters.ownedPresentationImages;
            incrementSaturating(counters.admissionRevision);
        } else if (published_) {
            if (liveOrigin && liveOrigin->state_ == state_ && liveOrigin->id_ == originId_) {
                liveOrigin->returnDeviceLocked(bytes_);
            } else {
                counters.totalDeviceClaimBytes -= bytes_;
            }
            // An independent real release remains observable even when its existing grant is
            // replenished. Only explicit proved failed-attempt cleanup suppresses that event.
            incrementSaturating(counters.admissionRevision);
        } else {
            origin_->returnDeviceLocked(bytes_);
        }
        if (!identity_.deviceLocal) {
            counters.ownedNonlocalNativeHostBytes -= bytes_;
            counters.totalHostClaimBytes -= bytes_;
        }
        releasedOrigin = std::move(origin_);
        bytes_ = 0;
        originId_ = 0;
        published_ = false;
    }
    releasedOrigin.reset();
    liveOrigin.reset();
    publishedOrigin_.reset();
    state_.reset();
}

} // namespace bloom::render
