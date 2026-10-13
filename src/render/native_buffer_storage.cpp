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
    : state_(phase->state_), origin_(std::move(phase)), bytes_(bytes), identity_(identity),
      upload_(upload), processing_(processing) {}
NativeBufferStorageToken::NativeBufferStorageToken(NativeBufferStorageToken&& other) noexcept
    : state_(std::move(other.state_)), origin_(std::move(other.origin_)),
      bytes_(std::exchange(other.bytes_, 0)), identity_(other.identity_), upload_(other.upload_),
      processing_(other.processing_) {}
NativeBufferStorageToken&
NativeBufferStorageToken::operator=(NativeBufferStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        origin_ = std::move(other.origin_);
        bytes_ = std::exchange(other.bytes_, 0);
        identity_ = other.identity_;
        upload_ = other.upload_;
        processing_ = other.processing_;
    }
    return *this;
}
NativeBufferStorageToken::~NativeBufferStorageToken() { reset(); }

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
    if (actual > bytes_)
        return false;
    const auto returned = bytes_ - actual;
    bytes_ = actual;
    auto& counters = state_->counters;
    if (upload_)
        origin_->remainingUploadBytes_ += returned;
    origin_->remainingDeviceBytes_ += returned;
    counters.deviceReservedBytes += returned;
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

void NativeBufferStorageToken::reset() noexcept {
    if (!state_)
        return;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        auto& counters = state_->counters;
        counters.ownedNativeBufferBytes -= bytes_;
        --counters.ownedNativeBuffers;
        if (upload_)
            origin_->remainingUploadBytes_ += bytes_;
        origin_->remainingDeviceBytes_ += bytes_;
        counters.deviceReservedBytes += bytes_;
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
        releasedOrigin = std::move(origin_);
    }
    releasedOrigin.reset();
    state_.reset();
}

} // namespace bloom::render
