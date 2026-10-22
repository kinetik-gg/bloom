#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_image_storage.hpp>
#include <bloom/render/packed_host_retirement.hpp>

#include "gpu_processing_phase_budget_private.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace bloom::render {
using detail::incrementSaturating;
using detail::remaining;
namespace {
void retireHostBackingLocked(detail::GpuProcessingPhaseBudgetState& state,
                             const std::uint64_t total) noexcept {
    auto& backing = state.rootedHostBacking;
    backing.materializedBytes -= total;
    if (total > std::numeric_limits<std::uint64_t>::max() - backing.retiredMaterializedBytes) {
        backing.retiredMaterializedBytes = std::numeric_limits<std::uint64_t>::max();
        backing.retiredBytesSaturated = true;
    } else {
        backing.retiredMaterializedBytes += total;
    }
    const auto revoked = std::min(total, state.hostBackingCreditRemaining);
    state.hostBackingCreditRemaining -= revoked;
    if (revoked != 0 && state.counters.hostCapacityBytes) {
        auto& capacity = *state.counters.hostCapacityBytes;
        const auto reduced = capacity > revoked ? capacity - revoked : 0;
        if (reduced != capacity) {
            capacity = reduced;
            incrementSaturating(state.counters.admissionRevision);
        }
    }
}
} // namespace

GpuProcessingPhaseLease::GpuProcessingPhaseLease(
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state) noexcept
    : state_(std::move(state)) {}

GpuProcessingPhaseLease::~GpuProcessingPhaseLease() {
    if (id_ == 0) {
        return;
    }
    const std::lock_guard lock(state_->mutex);
    auto& counters = state_->counters;
    counters.hostReservedBytes -= remainingHostBytes_;
    counters.totalHostClaimBytes -= remainingHostBytes_;
    counters.deviceReservedBytes -= remainingDeviceBytes_;
    counters.totalDeviceClaimBytes -= remainingDeviceBytes_;
    --counters.admittedPhases;
    incrementSaturating(counters.admissionRevision);
    if (unproven_) {
        --counters.unprovenPhases;
    }
}

std::uint64_t GpuProcessingPhaseLease::revisionAfterOwnRelease() const noexcept {
    return admissionRevision_ == std::numeric_limits<std::uint64_t>::max() ? admissionRevision_
                                                                           : admissionRevision_ + 1;
}

bool GpuProcessingPhaseLease::isUnproven() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return unproven_;
}

void GpuProcessingPhaseLease::markUnproven() noexcept {
    const std::lock_guard lock(state_->mutex);
    if (id_ != 0 && !unproven_) {
        unproven_ = true;
        ++state_->counters.unprovenPhases;
    }
}

PackedHostStorageToken::PackedHostStorageToken(
    std::shared_ptr<PackedHostStorageAuthority> authority, const std::uint64_t bytes,
    const std::uint64_t metadataBytes) noexcept
    : authority_(std::move(authority)), bytes_(bytes), metadataBytes_(metadataBytes) {}
PackedHostStorageToken::PackedHostStorageToken(PackedHostStorageToken&& other) noexcept
    : authority_(std::move(other.authority_)), bytes_(std::exchange(other.bytes_, 0)),
      metadataBytes_(std::exchange(other.metadataBytes_, 0)),
      materialized_(std::exchange(other.materialized_, false)) {}
PackedHostStorageToken& PackedHostStorageToken::operator=(PackedHostStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        authority_ = std::move(other.authority_);
        bytes_ = std::exchange(other.bytes_, 0);
        metadataBytes_ = std::exchange(other.metadataBytes_, 0);
        materialized_ = std::exchange(other.materialized_, false);
    }
    return *this;
}
PackedHostStorageToken::~PackedHostStorageToken() { reset(); }
void PackedHostStorageToken::reset() noexcept {
    if (authority_) {
        authority_->release(bytes_, metadataBytes_, materialized_);
        authority_.reset();
        bytes_ = 0;
        metadataBytes_ = 0;
        materialized_ = false;
    }
}
bool PackedHostStorageToken::published() const noexcept {
    return authority_ && authority_->snapshot().published;
}
void PackedHostStorageToken::markMaterialized() noexcept {
    if (authority_ && !materialized_) {
        authority_->materialize(bytes_, metadataBytes_);
        materialized_ = true;
    }
}
void PackedHostStorageToken::retireBeforeFree() noexcept {
    if (authority_ && materialized_) {
        authority_->retireMaterialized(bytes_, metadataBytes_);
        materialized_ = false;
    }
}
PackedHostStorageAuthority::PackedHostStorageAuthority(
    const std::shared_ptr<GpuProcessingPhaseLease>& phase, const std::uint64_t limit,
    const HostPixelStorageDomain domain) noexcept
    : state_(phase->state_), origin_(phase), domain_(domain), originId_(phase->id()),
      limit_(limit) {}
std::shared_ptr<PackedHostStorageAuthority>
PackedHostStorageAuthority::create(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                   const std::uint64_t limit, const HostPixelStorageDomain domain) {
    if (!phase) {
        return {};
    }
    return std::shared_ptr<PackedHostStorageAuthority>(
        new PackedHostStorageAuthority(phase, limit, domain));
}
std::optional<PackedHostStorageToken>
PackedHostStorageAuthority::reserve(const std::uint64_t bytes,
                                    const std::uint64_t metadataBytes) noexcept {
    if (bytes == 0) {
        return {};
    }
    auto self = weak_from_this().lock();
    auto origin = origin_.lock();
    if (!self || !origin) {
        return {};
    }
    const std::lock_guard lock(state_->mutex);
    if (published_ || (domain_ != HostPixelStorageDomain::PackedPreview && origin->unproven_) ||
        bytes_ > limit_ || bytes > limit_ - bytes_ || bytes > origin->remainingOutputHostBytes_ ||
        metadataBytes > origin->remainingOutputHostBytes_ - bytes) {
        return {};
    }
    // Both counters change under one lock; the total never changes during an ownership handoff.
    origin->remainingOutputHostBytes_ -= bytes + metadataBytes;
    origin->remainingHostBytes_ -= bytes + metadataBytes;
    state_->counters.hostReservedBytes -= bytes + metadataBytes;
    if (domain_ == HostPixelStorageDomain::PackedPreview) {
        state_->counters.ownedPackedHostBytes += bytes;
        state_->counters.ownedPackedHostMetadataBytes += metadataBytes;
        ++state_->counters.ownedPackedHostAllocations;
    } else {
        if (domain_ == HostPixelStorageDomain::FinalProcess)
            state_->counters.ownedFinalProcessHostBytes += bytes;
        else
            state_->counters.ownedFinalEncodedHostBytes += bytes;
        state_->counters.ownedFinalHostMetadataBytes += metadataBytes;
        ++state_->counters.ownedFinalHostAllocations;
    }
    bytes_ += bytes;
    metadataBytes_ += metadataBytes;
    retainedOrigin_ = std::move(origin);
    return PackedHostStorageToken(std::move(self), bytes, metadataBytes);
}
std::shared_ptr<detail::PackedHostRetirementState>
PackedHostStorageAuthority::retirementState() const noexcept {
    return state_->retirement;
}
void PackedHostStorageAuthority::release(const std::uint64_t bytes,
                                         const std::uint64_t metadataBytes,
                                         const bool materialized) noexcept {
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (materialized)
            retireHostBackingLocked(*state_, bytes + metadataBytes);
        bytes_ -= bytes;
        metadataBytes_ -= metadataBytes;
        if (domain_ == HostPixelStorageDomain::PackedPreview) {
            state_->counters.ownedPackedHostBytes -= bytes;
            state_->counters.ownedPackedHostMetadataBytes -= metadataBytes;
            --state_->counters.ownedPackedHostAllocations;
        } else {
            if (domain_ == HostPixelStorageDomain::FinalProcess)
                state_->counters.ownedFinalProcessHostBytes -= bytes;
            else
                state_->counters.ownedFinalEncodedHostBytes -= bytes;
            state_->counters.ownedFinalHostMetadataBytes -= metadataBytes;
            --state_->counters.ownedFinalHostAllocations;
        }
        if (published_) {
            state_->counters.totalHostClaimBytes -= bytes + metadataBytes;
            incrementSaturating(state_->counters.admissionRevision);
        } else {
            retainedOrigin_->remainingOutputHostBytes_ += bytes + metadataBytes;
            retainedOrigin_->remainingHostBytes_ += bytes + metadataBytes;
            state_->counters.hostReservedBytes += bytes + metadataBytes;
            if (bytes_ == 0) {
                releasedOrigin = std::move(retainedOrigin_);
            }
        }
    }
    // The final phase alias can run its destructor, which takes the same mutex.
    releasedOrigin.reset();
}
void PackedHostStorageAuthority::materialize(const std::uint64_t bytes,
                                             const std::uint64_t metadataBytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    // These bytes already belong to the bounded total claim; this is only a sampling proof.
    state_->rootedHostBacking.materializedBytes += bytes + metadataBytes;
}
void PackedHostStorageAuthority::retireMaterialized(const std::uint64_t bytes,
                                                    const std::uint64_t metadataBytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    retireHostBackingLocked(*state_, bytes + metadataBytes);
}
bool PackedHostStorageAuthority::promote() noexcept {
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (published_) {
            return true;
        }
        if (retainedOrigin_ && retainedOrigin_->unproven_) {
            return false;
        }
        published_ = true;
        releasedOrigin = std::move(retainedOrigin_);
    }
    releasedOrigin.reset();
    return true;
}
bool PackedHostStorageAuthority::promoteFinalProduct(
    const std::shared_ptr<PackedHostStorageAuthority>& process,
    const std::shared_ptr<PackedHostStorageAuthority>& encoded) noexcept {
    if (!process || process->domain_ != HostPixelStorageDomain::FinalProcess ||
        (encoded &&
         (encoded->domain_ != HostPixelStorageDomain::FinalEncoded ||
          encoded->state_ != process->state_ || encoded->originId_ != process->originId_)))
        return false;
    std::shared_ptr<GpuProcessingPhaseLease> releasedProcess;
    std::shared_ptr<GpuProcessingPhaseLease> releasedEncoded;
    {
        const std::lock_guard lock(process->state_->mutex);
        if (process->bytes_ == 0 ||
            (encoded &&
             (encoded->bytes_ == 0 || process->published_ != encoded->published_ ||
              (!process->published_ && process->retainedOrigin_ != encoded->retainedOrigin_))) ||
            (process->retainedOrigin_ && process->retainedOrigin_->unproven_) ||
            (encoded && encoded->retainedOrigin_ && encoded->retainedOrigin_->unproven_))
            return false;
        process->published_ = true;
        releasedProcess = std::move(process->retainedOrigin_);
        if (encoded) {
            encoded->published_ = true;
            releasedEncoded = std::move(encoded->retainedOrigin_);
        }
    }
    releasedProcess.reset();
    releasedEncoded.reset();
    return true;
}
PackedHostStorageSnapshot PackedHostStorageAuthority::snapshot() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return {.bytes = bytes_, .metadataBytes = metadataBytes_, .published = published_};
}

GpuProcessingPhaseBudget::GpuProcessingPhaseBudget(
    const std::size_t requestCapacity,
    const std::shared_ptr<PackedHostRetirementOwner>& retirementOwner)
    : state_(std::make_shared<detail::GpuProcessingPhaseBudgetState>()) {
    if (requestCapacity == 0 || requestCapacity > 1024) {
        throw std::invalid_argument("processing phase request capacity must be in [1, 1024]");
    }
    state_->counters.requestCapacity = requestCapacity;
    state_->retirement = retirementOwner ? retirementOwner->producerState() : nullptr;
}

void GpuProcessingPhaseBudget::setHostCapacity(
    const std::optional<std::uint64_t> bytes, const bool ready,
    const std::chrono::steady_clock::time_point validUntil) noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto beforeBytes = state_->counters.hostCapacityBytes;
    const bool beforeReady = state_->counters.hostReady;
    if (bytes) {
        state_->counters.hostCapacityBytes = bytes;
    }
    state_->counters.hostReady = ready && bytes.has_value();
    state_->hostValidUntil = validUntil;
    if (beforeBytes != state_->counters.hostCapacityBytes ||
        beforeReady != state_->counters.hostReady)
        incrementSaturating(state_->counters.admissionRevision);
}
void GpuProcessingPhaseBudget::setSampledHostCapacity(
    const std::optional<std::uint64_t> bytes, const bool ready,
    const std::chrono::steady_clock::time_point validUntil, const std::uint64_t creditBytes,
    const std::uint64_t retiredBytesAtSample) noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto beforeBytes = state_->counters.hostCapacityBytes;
    const bool beforeReady = state_->counters.hostReady;
    const auto& backing = state_->rootedHostBacking;
    const auto retired =
        backing.retiredBytesSaturated || backing.retiredMaterializedBytes < retiredBytesAtSample
            ? creditBytes
            : std::min(creditBytes, backing.retiredMaterializedBytes - retiredBytesAtSample);
    state_->hostBackingCreditRemaining = creditBytes - retired;
    if (bytes) {
        state_->counters.hostCapacityBytes = *bytes > retired ? *bytes - retired : 0;
    }
    state_->counters.hostReady = ready && bytes.has_value();
    state_->hostValidUntil = validUntil;
    if (beforeBytes != state_->counters.hostCapacityBytes ||
        beforeReady != state_->counters.hostReady)
        incrementSaturating(state_->counters.admissionRevision);
}
void GpuProcessingPhaseBudget::tightenHostCapacity(
    const std::optional<std::uint64_t> upperBound, const bool closeReadiness,
    const std::chrono::steady_clock::time_point validUntil) noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto beforeBytes = state_->counters.hostCapacityBytes;
    const bool beforeReady = state_->counters.hostReady;
    if (upperBound && state_->counters.hostCapacityBytes)
        state_->counters.hostCapacityBytes =
            std::min(*upperBound, *state_->counters.hostCapacityBytes);
    if (closeReadiness) {
        state_->counters.hostReady = false;
        state_->hostValidUntil = validUntil;
    }
    if (beforeBytes != state_->counters.hostCapacityBytes ||
        beforeReady != state_->counters.hostReady)
        incrementSaturating(state_->counters.admissionRevision);
}
void GpuProcessingPhaseBudget::setDeviceCapacity(const std::optional<std::uint64_t> bytes,
                                                 const bool ready) noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto beforeBytes = state_->counters.deviceCapacityBytes;
    const bool beforeReady = state_->counters.deviceReady;
    if (bytes) {
        state_->counters.deviceCapacityBytes = bytes;
    }
    state_->counters.deviceReady = ready && bytes.has_value();
    if (beforeBytes != state_->counters.deviceCapacityBytes ||
        beforeReady != state_->counters.deviceReady)
        incrementSaturating(state_->counters.admissionRevision);
}
GpuProcessingPhaseSnapshot GpuProcessingPhaseBudget::snapshot() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->counters;
}
RootedHostBackingSnapshot GpuProcessingPhaseBudget::rootedHostBackingSnapshot() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->rootedHostBacking;
}

GpuProcessingPhaseGrant
GpuProcessingPhaseBudget::reserve(const GpuProcessingPhaseRequest& request) noexcept {
    if (request.minimumSourceBytes > request.requestedSourceBytes ||
        request.minimumExecutorBytes > request.requestedExecutorBytes ||
        (request.sourceFree && request.minimumSourceBytes != 0) ||
        (!request.native &&
         (request.deviceWorkingBytes != 0 || request.requestedExecutorBytes != 0 ||
          request.minimumExecutorBytes != 0))) {
        return {};
    }
    std::shared_ptr<GpuProcessingPhaseLease> lease;
    try {
        // Allocation precedes the lock. Failure cannot consume either half of the paired grant.
        lease = std::shared_ptr<GpuProcessingPhaseLease>(new GpuProcessingPhaseLease(state_));
    } catch (...) {
        return {
            .status = GpuProcessingPhaseAdmission::AllocationFailure, .domain = {}, .lease = {}};
    }
    const std::lock_guard lock(state_->mutex);
    auto& counters = state_->counters;
    const auto refused = [&](const GpuProcessingPhaseDomain domain) {
        incrementSaturating(counters.budgetRefusals);
        return GpuProcessingPhaseGrant{.status = GpuProcessingPhaseAdmission::BudgetRefused,
                                       .domain = domain,
                                       .lease = {},
                                       .admissionRevision = counters.admissionRevision};
    };
    if (counters.hostReady && counters.hostCapacityBytes && *counters.hostCapacityBytes == 0) {
        return refused(GpuProcessingPhaseDomain::Host);
    }
    if (request.native && counters.deviceReady && counters.deviceCapacityBytes &&
        *counters.deviceCapacityBytes == 0) {
        return refused(GpuProcessingPhaseDomain::Device);
    }
    if (!counters.hostReady || !counters.hostCapacityBytes) {
        return {.status = GpuProcessingPhaseAdmission::AwaitingCapacity,
                .domain = GpuProcessingPhaseDomain::Host,
                .lease = {},
                .admissionRevision = counters.admissionRevision};
    }
    if (request.native && (!counters.deviceReady || !counters.deviceCapacityBytes)) {
        return {.status = GpuProcessingPhaseAdmission::AwaitingCapacity,
                .domain = GpuProcessingPhaseDomain::Device,
                .lease = {},
                .admissionRevision = counters.admissionRevision};
    }
    if (counters.admittedPhases >= counters.requestCapacity) {
        return refused(GpuProcessingPhaseDomain::RequestCount);
    }
    if (state_->nextId == 0) {
        return {.status = GpuProcessingPhaseAdmission::SerialExhausted, .domain = {}, .lease = {}};
    }
    const std::uint64_t hostRemaining =
        remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes);
    if (request.fixedHostBytes > hostRemaining) {
        return refused(GpuProcessingPhaseDomain::Host);
    }
    const std::uint64_t sourceRoom = (hostRemaining - request.fixedHostBytes) / 3;
    const std::uint64_t sourceBytes =
        request.sourceFree ? 0 : std::min(request.requestedSourceBytes, sourceRoom);
    if (sourceBytes < request.minimumSourceBytes ||
        (!request.sourceFree && request.requestedSourceBytes != 0 && sourceBytes == 0)) {
        return refused(GpuProcessingPhaseDomain::Host);
    }
    std::uint64_t executorBytes = 0;
    if (request.native) {
        const std::uint64_t deviceRemaining =
            remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes);
        if (request.deviceWorkingBytes > deviceRemaining) {
            return refused(GpuProcessingPhaseDomain::Device);
        }
        const std::uint64_t executorRoom = deviceRemaining - request.deviceWorkingBytes;
        executorBytes = std::min(request.requestedExecutorBytes, executorRoom);
        if (executorBytes < request.minimumExecutorBytes ||
            (request.requestedExecutorBytes != 0 && executorBytes == 0)) {
            return refused(GpuProcessingPhaseDomain::Device);
        }
    }
    // Division/subtraction bounds above prove these additions cannot overflow capacity.
    lease->hostBytes_ = request.fixedHostBytes + 3 * sourceBytes;
    lease->remainingHostBytes_ = lease->hostBytes_;
    lease->remainingOutputHostBytes_ = request.fixedHostBytes;
    lease->remainingUploadBytes_ = sourceBytes;
    lease->deviceBytes_ = request.native ? request.deviceWorkingBytes + executorBytes : 0;
    lease->remainingDeviceBytes_ = lease->deviceBytes_;
    lease->sourceBytes_ = sourceBytes;
    lease->executorBytes_ = executorBytes;
    lease->id_ = state_->nextId;
    lease->admissionRevision_ = counters.admissionRevision;
    state_->nextId =
        state_->nextId == std::numeric_limits<std::uint64_t>::max() ? 0 : state_->nextId + 1;
    counters.hostReservedBytes += lease->hostBytes_;
    counters.totalHostClaimBytes += lease->hostBytes_;
    counters.deviceReservedBytes += lease->deviceBytes_;
    counters.totalDeviceClaimBytes += lease->deviceBytes_;
    ++counters.admittedPhases;
    counters.peakHostReservedBytes =
        std::max(counters.peakHostReservedBytes, counters.hostReservedBytes);
    counters.peakDeviceReservedBytes =
        std::max(counters.peakDeviceReservedBytes, counters.deviceReservedBytes);
    return {.status = GpuProcessingPhaseAdmission::Accepted,
            .domain = GpuProcessingPhaseDomain::None,
            .lease = std::move(lease),
            .admissionRevision = counters.admissionRevision};
}
} // namespace bloom::render
