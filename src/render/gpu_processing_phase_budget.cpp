#include <bloom/render/gpu_processing_phase_budget.hpp>

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
    counters.hostReservedBytes -= hostBytes_;
    counters.deviceReservedBytes -= deviceBytes_;
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

void GpuProcessingPhaseLease::markUnproven() noexcept {
    const std::lock_guard lock(state_->mutex);
    if (id_ != 0 && !unproven_) {
        unproven_ = true;
        ++state_->counters.unprovenPhases;
    }
}

GpuProcessingPhaseBudget::GpuProcessingPhaseBudget(const std::size_t requestCapacity)
    : state_(std::make_shared<detail::GpuProcessingPhaseBudgetState>()) {
    if (requestCapacity == 0 || requestCapacity > 1024) {
        throw std::invalid_argument("processing phase request capacity must be in [1, 1024]");
    }
    state_->counters.requestCapacity = requestCapacity;
}

void GpuProcessingPhaseBudget::setHostCapacity(const std::optional<std::uint64_t> bytes,
                                               const bool ready) noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto beforeBytes = state_->counters.hostCapacityBytes;
    const bool beforeReady = state_->counters.hostReady;
    if (bytes) {
        state_->counters.hostCapacityBytes = bytes;
    }
    state_->counters.hostReady = ready && bytes.has_value();
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
        remaining(*counters.hostCapacityBytes, counters.hostReservedBytes);
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
            remaining(*counters.deviceCapacityBytes, counters.deviceReservedBytes);
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
    lease->deviceBytes_ = request.native ? request.deviceWorkingBytes + executorBytes : 0;
    lease->sourceBytes_ = sourceBytes;
    lease->executorBytes_ = executorBytes;
    lease->id_ = state_->nextId;
    lease->admissionRevision_ = counters.admissionRevision;
    state_->nextId =
        state_->nextId == std::numeric_limits<std::uint64_t>::max() ? 0 : state_->nextId + 1;
    counters.hostReservedBytes += lease->hostBytes_;
    counters.deviceReservedBytes += lease->deviceBytes_;
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
