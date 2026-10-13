#include <bloom/render/processing_host_storage.hpp>

#include "gpu_processing_phase_budget_private.hpp"

namespace bloom::render {
ProcessingHostStorageToken::ProcessingHostStorageToken(
    std::shared_ptr<GpuProcessingPhaseLease> phase, const std::uint64_t bytes,
    const std::uint64_t metadata) noexcept
    : state_(phase->state_), origin_(std::move(phase)), bytes_(bytes), metadataBytes_(metadata) {}
ProcessingHostStorageToken::ProcessingHostStorageToken(ProcessingHostStorageToken&& other) noexcept
    : state_(std::move(other.state_)), origin_(std::move(other.origin_)),
      bytes_(std::exchange(other.bytes_, 0)),
      metadataBytes_(std::exchange(other.metadataBytes_, 0)) {}
ProcessingHostStorageToken&
ProcessingHostStorageToken::operator=(ProcessingHostStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        origin_ = std::move(other.origin_);
        bytes_ = std::exchange(other.bytes_, 0);
        metadataBytes_ = std::exchange(other.metadataBytes_, 0);
    }
    return *this;
}
ProcessingHostStorageToken::~ProcessingHostStorageToken() { reset(); }
ProcessingHostStorageGrant
ProcessingHostStorageToken::reserve(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                    const std::uint64_t bytes, const std::uint64_t metadata,
                                    const std::chrono::steady_clock::time_point now) noexcept {
    if (!phase || bytes == 0 || metadata > std::numeric_limits<std::uint64_t>::max() - bytes)
        return {};
    const std::lock_guard lock(phase->state_->mutex);
    auto& state = *phase->state_;
    auto& counters = state.counters;
    if (phase->id_ == 0 || phase->unproven_)
        return {};
    const auto refuse = [&](const GpuProcessingPhaseAdmission status) {
        if (status == GpuProcessingPhaseAdmission::BudgetRefused)
            detail::incrementSaturating(counters.budgetRefusals);
        return ProcessingHostStorageGrant{.status = status,
                                          .domain = GpuProcessingPhaseDomain::Host,
                                          .token = {},
                                          .revisionAfterOwnRelease =
                                              counters.admissionRevision == UINT64_MAX
                                                  ? counters.admissionRevision
                                                  : counters.admissionRevision + 1};
    };
    if (counters.hostReady && now > state.hostValidUntil) {
        counters.hostReady = false;
        detail::incrementSaturating(counters.admissionRevision);
    }
    if (!counters.hostReady || !counters.hostCapacityBytes || now > state.hostValidUntil)
        return refuse(GpuProcessingPhaseAdmission::AwaitingCapacity);
    const auto total = bytes + metadata;
    if (total > detail::remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes))
        return refuse(GpuProcessingPhaseAdmission::BudgetRefused);
    counters.ownedProcessingHostBytes += bytes;
    counters.ownedProcessingHostMetadataBytes += metadata;
    ++counters.ownedProcessingHostAllocations;
    counters.totalHostClaimBytes += total;
    return {.status = GpuProcessingPhaseAdmission::Accepted,
            .domain = GpuProcessingPhaseDomain::None,
            .token = ProcessingHostStorageToken(phase, bytes, metadata)};
}
void ProcessingHostStorageToken::reset() noexcept {
    if (!state_)
        return;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        auto& counters = state_->counters;
        counters.ownedProcessingHostBytes -= bytes_;
        counters.ownedProcessingHostMetadataBytes -= metadataBytes_;
        --counters.ownedProcessingHostAllocations;
        counters.totalHostClaimBytes -= bytes_ + metadataBytes_;
        bytes_ = 0;
        metadataBytes_ = 0;
        releasedOrigin = std::move(origin_);
    }
    releasedOrigin.reset();
    state_.reset();
}
} // namespace bloom::render
