#include <bloom/render/native_buffer_storage.hpp>

#include "gpu_processing_phase_budget_private.hpp"

#include <utility>

namespace bloom::render {
using detail::incrementSaturating;
using detail::remaining;

NativeBufferStorageToken::NativeBufferStorageToken(std::shared_ptr<GpuProcessingPhaseLease> phase,
                                                   const std::uint64_t bytes,
                                                   const NativeBufferStorageIdentity identity,
                                                   const bool upload,
                                                   const bool processing) noexcept
    : state_(phase->state_), origin_(std::move(phase)), originId_(origin_->id_), bytes_(bytes),
      identity_(identity), upload_(upload), processing_(processing) {}
NativeBufferStorageToken::NativeBufferStorageToken(GpuProcessingRootIdentity root,
                                                   const std::uint64_t bytes,
                                                   NativeBufferStorageIdentity identity) noexcept
    : state_(std::const_pointer_cast<detail::GpuProcessingPhaseBudgetState>(std::move(root))),
      bytes_(bytes), identity_(identity), processing_(true), presentation_(true) {}
NativeBufferStorageToken::NativeBufferStorageToken(NativeBufferStorageToken&& other) noexcept
    : state_(std::move(other.state_)), origin_(std::move(other.origin_)),
      publishedOrigin_(std::move(other.publishedOrigin_)),
      originId_(std::exchange(other.originId_, 0)), bytes_(std::exchange(other.bytes_, 0)),
      identity_(other.identity_), upload_(other.upload_), processing_(other.processing_),
      published_(std::exchange(other.published_, false)),
      presentation_(std::exchange(other.presentation_, false)) {}
NativeBufferStorageToken&
NativeBufferStorageToken::operator=(NativeBufferStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        origin_ = std::move(other.origin_);
        publishedOrigin_ = std::move(other.publishedOrigin_);
        originId_ = std::exchange(other.originId_, 0);
        bytes_ = std::exchange(other.bytes_, 0);
        identity_ = other.identity_;
        upload_ = other.upload_;
        processing_ = other.processing_;
        published_ = std::exchange(other.published_, false);
        presentation_ = std::exchange(other.presentation_, false);
    }
    return *this;
}
NativeBufferStorageToken::~NativeBufferStorageToken() { reset(); }

NativeBufferStorageGrant NativeBufferStorageToken::reservePresentation(
    const GpuProcessingRootIdentity& root, const std::uint64_t bytes,
    const NativeBufferStorageIdentity identity,
    const std::chrono::steady_clock::time_point now) noexcept {
    if (!root || bytes == 0 || identity.allocatorGeneration == 0)
        return {};
    auto state = std::const_pointer_cast<detail::GpuProcessingPhaseBudgetState>(root);
    const auto admission =
        detail::admitPresentation(*state, bytes, identity.deviceLocal, false, now);
    if (admission.status != GpuProcessingPhaseAdmission::Accepted)
        return {.status = admission.status,
                .domain = admission.domain,
                .token = {},
                .revisionAfterOwnRelease = admission.revision};
    return {.status = admission.status,
            .domain = admission.domain,
            .token = NativeBufferStorageToken(root, bytes, identity)};
}

NativeBufferStorageGrant
NativeBufferStorageToken::reserve(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                  const std::uint64_t bytes,
                                  const NativeBufferStorageIdentity identity) noexcept {
    return reserveInPartition(phase, bytes, identity, false);
}
NativeBufferStorageGrant
NativeBufferStorageToken::reserveUpload(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                        const std::uint64_t bytes,
                                        const NativeBufferStorageIdentity identity) noexcept {
    return reserveInPartition(phase, bytes, identity, true);
}
NativeBufferStorageGrant NativeBufferStorageToken::reserveProcessing(
    const std::shared_ptr<GpuProcessingPhaseLease>& phase, const std::uint64_t bytes,
    const NativeBufferStorageIdentity identity,
    const std::chrono::steady_clock::time_point now) noexcept {
    return reserveInPartition(phase, bytes, identity, false, true, now);
}
NativeBufferStorageGrant NativeBufferStorageToken::reserveInPartition(
    const std::shared_ptr<GpuProcessingPhaseLease>& phase, const std::uint64_t bytes,
    const NativeBufferStorageIdentity identity, const bool upload, const bool processing,
    const std::chrono::steady_clock::time_point now) noexcept {
    if (!phase || bytes == 0 || identity.allocatorGeneration == 0)
        return {};
    const std::lock_guard lock(phase->state_->mutex);
    auto& counters = phase->state_->counters;
    if (phase->id_ == 0 || phase->deviceBytes_ == 0 || phase->unproven_)
        return {};
    const auto refuse = [&](const GpuProcessingPhaseDomain domain,
                            const GpuProcessingPhaseAdmission status =
                                GpuProcessingPhaseAdmission::BudgetRefused) {
        if (status == GpuProcessingPhaseAdmission::BudgetRefused)
            incrementSaturating(counters.budgetRefusals);
        return NativeBufferStorageGrant{.status = status,
                                        .domain = domain,
                                        .token = {},
                                        .revisionAfterOwnRelease =
                                            counters.admissionRevision ==
                                                    std::numeric_limits<std::uint64_t>::max()
                                                ? counters.admissionRevision
                                                : counters.admissionRevision + 1};
    };
    if (bytes > phase->remainingDeviceBytes_)
        return refuse(GpuProcessingPhaseDomain::Device);
    if ((upload && bytes > phase->remainingUploadBytes_) ||
        (!upload && !processing && !identity.deviceLocal &&
         bytes > phase->remainingOutputHostBytes_))
        return refuse(GpuProcessingPhaseDomain::Host);
    if (processing && !identity.deviceLocal) {
        auto& state = *phase->state_;
        if (counters.hostReady && now > state.hostValidUntil) {
            counters.hostReady = false;
            incrementSaturating(counters.admissionRevision);
        }
        if (!counters.hostReady || !counters.hostCapacityBytes || now > state.hostValidUntil)
            return refuse(GpuProcessingPhaseDomain::Host,
                          GpuProcessingPhaseAdmission::AwaitingCapacity);
        if (bytes > remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes))
            return refuse(GpuProcessingPhaseDomain::Host);
    }
    // Native ownership transfers existing debit. Only nonlocal processing adds host credit;
    // output/upload convert protected host debit. No half-transaction escapes on refusal.
    if (upload)
        phase->remainingUploadBytes_ -= bytes;
    phase->remainingDeviceBytes_ -= bytes;
    counters.deviceReservedBytes -= bytes;
    counters.ownedNativeBufferBytes += bytes;
    ++counters.ownedNativeBuffers;
    if (!identity.deviceLocal) {
        if (processing) {
            counters.totalHostClaimBytes += bytes;
        } else {
            if (!upload)
                phase->remainingOutputHostBytes_ -= bytes;
            phase->remainingHostBytes_ -= bytes;
            counters.hostReservedBytes -= bytes;
        }
        counters.ownedNonlocalNativeBufferHostBytes += bytes;
    }
    return {.status = GpuProcessingPhaseAdmission::Accepted,
            .domain = GpuProcessingPhaseDomain::None,
            .token = NativeBufferStorageToken(phase, bytes, identity, upload, processing)};
}

bool NativeBufferStorageToken::reconcile(const std::uint64_t actual,
                                         const NativeBufferStorageIdentity identity) noexcept {
    if (!state_ || actual == 0 || identity != identity_)
        return false;
    const std::lock_guard lock(state_->mutex);
    if (published_ || (!origin_ && !presentation_) || actual > bytes_)
        return false;
    const auto returned = bytes_ - actual;
    bytes_ = actual;
    auto& counters = state_->counters;
    if (upload_)
        origin_->remainingUploadBytes_ += returned;
    if (presentation_) {
        counters.totalDeviceClaimBytes -= returned;
        counters.ownedPresentationBufferBytes -= returned;
    } else {
        origin_->remainingDeviceBytes_ += returned;
        counters.deviceReservedBytes += returned;
    }
    counters.ownedNativeBufferBytes -= returned;
    if (!identity_.deviceLocal) {
        if (processing_) {
            counters.totalHostClaimBytes -= returned;
        } else {
            if (!upload_)
                origin_->remainingOutputHostBytes_ += returned;
            origin_->remainingHostBytes_ += returned;
            counters.hostReservedBytes += returned;
        }
        counters.ownedNonlocalNativeBufferHostBytes -= returned;
    }
    return true;
}

bool NativeBufferStorageToken::canPromote() const noexcept {
    if (!state_ || !processing_ || upload_)
        return false;
    const std::lock_guard lock(state_->mutex);
    return presentation_ || published_ || (origin_ && !origin_->unproven_);
}

bool NativeBufferStorageToken::promote() noexcept {
    if (!state_ || !processing_ || upload_)
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

bool NativeBufferStorageToken::releaseFailedAttemptAfterBackingDestroyed(
    const std::shared_ptr<GpuProcessingPhaseLease>& phase) noexcept {
    if (!state_ || presentation_ || !processing_ || !phase || phase->state_ != state_ ||
        phase->id_ != originId_)
        return false;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (phase->unproven_)
            return false;
        auto& counters = state_->counters;
        counters.ownedNativeBufferBytes -= bytes_;
        --counters.ownedNativeBuffers;
        phase->remainingDeviceBytes_ += bytes_;
        counters.deviceReservedBytes += bytes_;
        if (!identity_.deviceLocal) {
            counters.ownedNonlocalNativeBufferHostBytes -= bytes_;
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

void NativeBufferStorageToken::reset() noexcept {
    if (!state_)
        return;
    auto liveOrigin = published_ ? publishedOrigin_.lock() : nullptr;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        auto& counters = state_->counters;
        counters.ownedNativeBufferBytes -= bytes_;
        --counters.ownedNativeBuffers;
        if (presentation_) {
            counters.totalDeviceClaimBytes -= bytes_;
            counters.ownedPresentationBufferBytes -= bytes_;
            --counters.ownedPresentationBuffers;
            incrementSaturating(counters.admissionRevision);
        } else if (published_) {
            if (liveOrigin && liveOrigin->state_ == state_ && liveOrigin->id_ == originId_) {
                liveOrigin->remainingDeviceBytes_ += bytes_;
                counters.deviceReservedBytes += bytes_;
            } else {
                counters.totalDeviceClaimBytes -= bytes_;
            }
            incrementSaturating(counters.admissionRevision);
        } else {
            if (upload_)
                origin_->remainingUploadBytes_ += bytes_;
            origin_->remainingDeviceBytes_ += bytes_;
            counters.deviceReservedBytes += bytes_;
        }
        if (!identity_.deviceLocal) {
            if (processing_) {
                counters.totalHostClaimBytes -= bytes_;
            } else {
                if (!upload_)
                    origin_->remainingOutputHostBytes_ += bytes_;
                origin_->remainingHostBytes_ += bytes_;
                counters.hostReservedBytes += bytes_;
            }
            counters.ownedNonlocalNativeBufferHostBytes -= bytes_;
        }
        bytes_ = 0;
        originId_ = 0;
        published_ = false;
        releasedOrigin = std::move(origin_);
    }
    releasedOrigin.reset();
    publishedOrigin_.reset();
    state_.reset();
}

} // namespace bloom::render
