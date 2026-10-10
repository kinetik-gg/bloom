#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_buffer_storage.hpp>
#include <bloom/render/native_image_storage.hpp>
#include <bloom/render/packed_host_retirement.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace bloom::render {
namespace detail {
struct GpuProcessingPhaseBudgetState final {
    mutable std::mutex mutex;
    GpuProcessingPhaseSnapshot counters;
    std::uint64_t nextId = 1;
    std::shared_ptr<PackedHostRetirementState> retirement;
    std::chrono::steady_clock::time_point hostValidUntil =
        std::chrono::steady_clock::time_point::max();
};
} // namespace detail
namespace {
[[nodiscard]] std::uint64_t remaining(const std::uint64_t cap,
                                      const std::uint64_t reserved) noexcept {
    return cap > reserved ? cap - reserved : 0;
}
void incrementSaturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
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

NativeBufferStorageToken::NativeBufferStorageToken(
    std::shared_ptr<GpuProcessingPhaseLease> phase, const std::uint64_t bytes,
    const NativeBufferStorageIdentity identity) noexcept
    : state_(phase->state_), origin_(std::move(phase)), bytes_(bytes), identity_(identity) {}
NativeBufferStorageToken::NativeBufferStorageToken(NativeBufferStorageToken&& other) noexcept
    : state_(std::move(other.state_)), origin_(std::move(other.origin_)),
      bytes_(std::exchange(other.bytes_, 0)), identity_(other.identity_) {}
NativeBufferStorageToken&
NativeBufferStorageToken::operator=(NativeBufferStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        origin_ = std::move(other.origin_);
        bytes_ = std::exchange(other.bytes_, 0);
        identity_ = other.identity_;
    }
    return *this;
}
NativeBufferStorageToken::~NativeBufferStorageToken() { reset(); }

NativeBufferStorageGrant
NativeBufferStorageToken::reserve(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                  const std::uint64_t bytes,
                                  const NativeBufferStorageIdentity identity) noexcept {
    if (!phase || bytes == 0 || identity.allocatorGeneration == 0)
        return {};
    const std::lock_guard lock(phase->state_->mutex);
    auto& counters = phase->state_->counters;
    if (phase->id_ == 0 || phase->deviceBytes_ == 0 || phase->unproven_)
        return {};
    const auto refuse = [&](const GpuProcessingPhaseDomain domain) {
        incrementSaturating(counters.budgetRefusals);
        return NativeBufferStorageGrant{.status = GpuProcessingPhaseAdmission::BudgetRefused,
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
    if (!identity.deviceLocal && bytes > phase->remainingHostBytes_)
        return refuse(GpuProcessingPhaseDomain::Host);
    // All additions transfer existing bounded debits atomically; no new capacity/readiness check
    // can revoke an admitted grant, and no half-transaction can escape on refusal.
    phase->remainingDeviceBytes_ -= bytes;
    counters.deviceReservedBytes -= bytes;
    counters.ownedNativeBufferBytes += bytes;
    ++counters.ownedNativeBuffers;
    if (!identity.deviceLocal) {
        phase->remainingHostBytes_ -= bytes;
        counters.hostReservedBytes -= bytes;
        counters.ownedNonlocalNativeBufferHostBytes += bytes;
    }
    return {.status = GpuProcessingPhaseAdmission::Accepted,
            .domain = GpuProcessingPhaseDomain::None,
            .token = NativeBufferStorageToken(phase, bytes, identity)};
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
    origin_->remainingDeviceBytes_ += returned;
    counters.deviceReservedBytes += returned;
    counters.ownedNativeBufferBytes -= returned;
    if (!identity_.deviceLocal) {
        origin_->remainingHostBytes_ += returned;
        counters.hostReservedBytes += returned;
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
        origin_->remainingDeviceBytes_ += bytes_;
        counters.deviceReservedBytes += bytes_;
        if (!identity_.deviceLocal) {
            origin_->remainingHostBytes_ += bytes_;
            counters.hostReservedBytes += bytes_;
            counters.ownedNonlocalNativeBufferHostBytes -= bytes_;
        }
        bytes_ = 0;
        releasedOrigin = std::move(origin_);
    }
    releasedOrigin.reset();
    state_.reset();
}

NativeImageStorageToken::NativeImageStorageToken(std::shared_ptr<GpuProcessingPhaseLease> phase,
                                                 const std::uint64_t bytes,
                                                 const NativeImageStorageIdentity identity) noexcept
    : state_(phase->state_), origin_(std::move(phase)), originId_(origin_->id_), bytes_(bytes),
      identity_(identity) {}
NativeImageStorageToken::NativeImageStorageToken(NativeImageStorageToken&& other) noexcept
    : state_(std::move(other.state_)), origin_(std::move(other.origin_)),
      publishedOrigin_(std::move(other.publishedOrigin_)),
      originId_(std::exchange(other.originId_, 0)), bytes_(std::exchange(other.bytes_, 0)),
      identity_(other.identity_), published_(std::exchange(other.published_, false)) {}
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
    }
    return *this;
}
NativeImageStorageToken::~NativeImageStorageToken() { reset(); }

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
    if (bytes > phase->remainingDeviceBytes_)
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
    phase->remainingDeviceBytes_ -= bytes;
    counters.deviceReservedBytes -= bytes;
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
    if (published_ || !origin_ || actual > bytes_)
        return false;
    const auto returned = bytes_ - actual;
    bytes_ = actual;
    origin_->remainingDeviceBytes_ += returned;
    auto& counters = state_->counters;
    counters.deviceReservedBytes += returned;
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
    if (!state_ || !phase || phase->state_ != state_ || phase->id_ != originId_)
        return false;
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (phase->unproven_)
            return false;
        auto& counters = state_->counters;
        counters.ownedNativeImageBytes -= bytes_;
        --counters.ownedNativeImages;
        counters.deviceReservedBytes += bytes_;
        phase->remainingDeviceBytes_ += bytes_;
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
        if (published_) {
            if (liveOrigin && liveOrigin->state_ == state_ && liveOrigin->id_ == originId_) {
                liveOrigin->remainingDeviceBytes_ += bytes_;
                counters.deviceReservedBytes += bytes_;
            } else {
                counters.totalDeviceClaimBytes -= bytes_;
            }
            // An independent real release remains observable even when its existing grant is
            // replenished. Only explicit proved failed-attempt cleanup suppresses that event.
            incrementSaturating(counters.admissionRevision);
        } else {
            origin_->remainingDeviceBytes_ += bytes_;
            counters.deviceReservedBytes += bytes_;
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

PackedHostStorageToken::PackedHostStorageToken(
    std::shared_ptr<PackedHostStorageAuthority> authority, const std::uint64_t bytes,
    const std::uint64_t metadataBytes) noexcept
    : authority_(std::move(authority)), bytes_(bytes), metadataBytes_(metadataBytes) {}
PackedHostStorageToken::PackedHostStorageToken(PackedHostStorageToken&& other) noexcept
    : authority_(std::move(other.authority_)), bytes_(std::exchange(other.bytes_, 0)),
      metadataBytes_(std::exchange(other.metadataBytes_, 0)) {}
PackedHostStorageToken& PackedHostStorageToken::operator=(PackedHostStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        authority_ = std::move(other.authority_);
        bytes_ = std::exchange(other.bytes_, 0);
        metadataBytes_ = std::exchange(other.metadataBytes_, 0);
    }
    return *this;
}
PackedHostStorageToken::~PackedHostStorageToken() { reset(); }
void PackedHostStorageToken::reset() noexcept {
    if (authority_) {
        authority_->release(bytes_, metadataBytes_);
        authority_.reset();
        bytes_ = 0;
        metadataBytes_ = 0;
    }
}
bool PackedHostStorageToken::published() const noexcept {
    return authority_ && authority_->snapshot().published;
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
        bytes_ > limit_ || bytes > limit_ - bytes_ || bytes > origin->remainingHostBytes_ ||
        metadataBytes > origin->remainingHostBytes_ - bytes) {
        return {};
    }
    // Both counters change under one lock; the total never changes during an ownership handoff.
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
                                         const std::uint64_t metadataBytes) noexcept {
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
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

GpuProcessingPhaseGrant
GpuProcessingPhaseBudget::reserve(const GpuProcessingPhaseRequest& request) noexcept {
    if (request.capacityShareDenominator == 0 ||
        request.minimumSourceBytes > request.requestedSourceBytes ||
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
    const std::uint64_t sourceShare =
        *counters.hostCapacityBytes / request.capacityShareDenominator;
    const std::uint64_t sourceBytes =
        request.sourceFree
            ? 0
            : std::min(request.requestedSourceBytes,
                       std::min(sourceRoom, std::max(sourceShare, request.minimumSourceBytes)));
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
        const std::uint64_t executorShare =
            *counters.deviceCapacityBytes / request.capacityShareDenominator;
        executorBytes =
            std::min(request.requestedExecutorBytes,
                     std::min(executorRoom, std::max(executorShare, request.minimumExecutorBytes)));
        if (executorBytes < request.minimumExecutorBytes ||
            (request.requestedExecutorBytes != 0 && executorBytes == 0)) {
            return refused(GpuProcessingPhaseDomain::Device);
        }
    }
    // Division/subtraction bounds above prove these additions cannot overflow capacity.
    lease->hostBytes_ = request.fixedHostBytes + 3 * sourceBytes;
    lease->remainingHostBytes_ = lease->hostBytes_;
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
