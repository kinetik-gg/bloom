#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <algorithm>
#include <limits>

namespace bloom::runtime {
namespace {
std::optional<std::uint64_t> add(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return {};
    }
    return left + right;
}
std::optional<std::uint64_t> multiply(const std::uint64_t value,
                                      const std::uint64_t factor) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() / factor) {
        return {};
    }
    return value * factor;
}
} // namespace
std::optional<render::GpuProcessingPhaseRequest>
processingPhaseRequest(const GpuProcessingPhaseEnvelope& envelope) noexcept {
    if (envelope.pixels == 0) {
        return {};
    }
    const auto outputs = multiply(
        envelope.pixels, envelope.output == GpuProcessingPhaseOutput::PackedPreview ? 8 : 48);
    const auto encoded = multiply(envelope.encodedBytes, 2);
    if (!outputs || !encoded) {
        return {};
    }
    const auto coverageOverlap = multiply(envelope.retainedCoverageByteLimit, 2);
    if (!coverageOverlap) {
        return {};
    }
    auto fixed = add(*outputs, envelope.metadataBytes);
    if (!fixed) {
        return {};
    }
    fixed = add(*fixed, *coverageOverlap);
    if (!fixed) {
        return {};
    }
    if (envelope.output == GpuProcessingPhaseOutput::FloatFinal) {
        fixed = add(*fixed, *encoded);
        if (!fixed) {
            return {};
        }
    }
    return render::GpuProcessingPhaseRequest{.fixedHostBytes = *fixed,
                                             .requestedSourceBytes = envelope.requestedSourceBytes,
                                             .minimumSourceBytes = envelope.minimumSourceBytes,
                                             .deviceWorkingBytes = envelope.deviceWorkingBytes,
                                             .requestedExecutorBytes =
                                                 envelope.requestedExecutorBytes.value_or(0),
                                             .minimumExecutorBytes = envelope.minimumExecutorBytes,
                                             .sourceFree = envelope.sourceFree,
                                             .native = envelope.native};
}
GpuProcessingPhasePolicy::GpuProcessingPhasePolicy(
    const std::size_t requestCapacity,
    const std::shared_ptr<render::PackedHostRetirementOwner>& retirementOwner,
    std::shared_ptr<TaskBudgetView> budgetView)
    : budget_(requestCapacity, retirementOwner), budgetView_(std::move(budgetView)) {}
void GpuProcessingPhasePolicy::applyHostSample(const MemoryBudgetState& sample,
                                               const std::size_t physicalBytes,
                                               const Clock::time_point now) noexcept {
    const std::lock_guard lock(mutex_);
    const bool newerDelivery = sample.sampleSerial > sampleSerial_;
    if (newerDelivery) {
        processSource_ = sample.process.source;
        processStatus_ = sample.process.status;
        sampleSerial_ = sample.sampleSerial;
    }
    const auto finite = minimumProcessCacheAllowance(sample.process, 0);
    if (!newerDelivery) {
        // Multiple composition roots may replay the same already-delivered neutral snapshot.
        // A duplicate never closes/reopens readiness or authorizes growth; reliable tighter
        // constraints remain admissible even on this path.
        processConstraintHistory_.recordFinite(sample.process, false);
        if (finite) {
            finiteProcessCeiling_ = finiteProcessCeiling_
                                        ? std::min<std::uint64_t>(*finiteProcessCeiling_, *finite)
                                        : *finite;
            const auto current = budget_.snapshot();
            if (current.hostCapacityBytes && *finite < *current.hostCapacityBytes) {
                budget_.setHostCapacity(finite, current.hostReady,
                                        hostSampleAt_.value_or(Clock::time_point::min()) +
                                            std::chrono::seconds(10));
            }
        }
        return;
    }
    // Failed/incomplete sampling may only tighten the last known constraint. No reusable cache
    // credit is manufactured; process usage already includes retained/native allocations.
    if (finite) {
        finiteProcessCeiling_ = finiteProcessCeiling_
                                    ? std::min<std::uint64_t>(*finiteProcessCeiling_, *finite)
                                    : *finite;
    }
    const bool complete = isCompleteProcessMemorySample(sample.process) ||
                          sample.process.status == ProcessMemoryStatus::Unsupported;
    const auto age =
        std::chrono::duration<long double>(now.time_since_epoch()).count() -
        std::chrono::duration<long double>(sample.process.sampledAt.time_since_epoch()).count();
    const bool recent = age >= 0 && age <= 10;
    const bool fresh = newerDelivery && sample.processSampleFresh && recent &&
                       (!hostSampleAt_ || sample.process.sampledAt > *hostSampleAt_);
    processConstraintHistory_.recordFinite(
        sample.process, complete && fresh && sample.process.status == ProcessMemoryStatus::Finite);
    if (complete && fresh && processConstraintHistory_.verifiesRemoval(sample.process)) {
        finiteProcessCeiling_.reset();
    }
    if (fresh && sample.process.status == ProcessMemoryStatus::Finite &&
        isCompleteProcessMemorySample(sample.process) && finite) {
        finiteProcessCeiling_ = finite;
    }
    std::optional<std::uint64_t> allowance;
    if (sample.machine.availableBytes) {
        const auto hostReserve =
            std::max(kMinimumHostMemoryReserve, physicalBytes / 100 * kHostMemoryReservePercent);
        allowance = *sample.machine.availableBytes > hostReserve
                        ? *sample.machine.availableBytes - hostReserve
                        : 0;
        if (finiteProcessCeiling_) {
            allowance.emplace(std::min(*allowance, *finiteProcessCeiling_));
        }
    } else if (finiteProcessCeiling_ && *finiteProcessCeiling_ == 0) {
        allowance = 0;
    }
    const bool ready = allowance && ((*allowance == 0 && recent) ||
                                     (complete && fresh && !sample.awaitingProcessSample));
    if (fresh) {
        hostSampleAt_ = sample.process.sampledAt;
    }
    budget_.setHostCapacity(allowance, ready,
                            hostSampleAt_.value_or(Clock::time_point::min()) +
                                std::chrono::seconds(10));
}
void GpuProcessingPhasePolicy::applyHostSample(const MemoryBudgetState& sample,
                                               const std::size_t physicalBytes,
                                               const MemoryBudgetAllocation& allocation,
                                               const Clock::time_point now) noexcept {
    applyHostSample(sample, physicalBytes, now);
    if (!budgetView_)
        return;
    const auto current = snapshot();
    if (current.sampleSerial != sample.sampleSerial)
        return;
    const auto host = current.reservations.hostCapacityBytes.value_or(0);
    const auto hostCeiling = static_cast<std::size_t>(
        std::min<std::uint64_t>(host, std::numeric_limits<std::size_t>::max()));
    // Configured shares are ceilings/weights, not permission to skip the ledger's timed host
    // recovery. Scale from the delivered LIVE processing cap, independently of reusable credit.
    const auto configured =
        std::max(static_cast<long double>(sample.configuredBytes),
                 static_cast<long double>(allocation.operationCacheByteBudget) +
                     static_cast<long double>(allocation.previewFrameCacheByteBudget));
    const auto live = std::min(sample.processingEffectiveBytes, allocation.usableByteBudget);
    const auto assigned = [configured, live](const std::size_t ceiling) {
        return configured == 0
                   ? std::size_t{0}
                   : static_cast<std::size_t>(static_cast<long double>(ceiling) *
                                              std::min(configured, static_cast<long double>(live)) /
                                              configured);
    };
    const auto operation = assigned(allocation.operationCacheByteBudget);
    const auto preview = assigned(allocation.previewFrameCacheByteBudget);
    const auto aggregate = std::min(hostCeiling, operation);
    const auto request = std::min(aggregate, gpuPreviewRequestByteAllowanceFor(preview));
    static_cast<void>(
        budgetView_->publish({.sampleSerial = current.sampleSerial,
                              .sampledAt = sample.process.sampledAt,
                              .ready = current.reservations.hostReady &&
                                       sample.processSampleFresh && !sample.awaitingProcessSample,
                              .assignedPreviewBytes = preview,
                              .assignedOperationBytes = operation,
                              .previewRequestBytes = request,
                              .aggregateGpuBytes = aggregate}));
}
bool GpuProcessingPhasePolicy::reportDevice(const std::uint64_t ownershipEpoch,
                                            const render::GpuAllocationBudget report) noexcept {
    if (ownershipEpoch == 0) {
        return false;
    }
    const std::lock_guard lock(mutex_);
    std::size_t index = 0;
    for (; index < deviceCount_; ++index) {
        if (devices_[index].ownershipEpoch == ownershipEpoch) {
            break;
        }
    }
    if (index == devices_.size()) {
        budget_.setDeviceCapacity(std::nullopt, false);
        return false;
    }
    if (index == deviceCount_) {
        ++deviceCount_;
    }
    devices_[index] = {.ownershipEpoch = ownershipEpoch, .budget = report};
    applyDeviceReportsLocked();
    return true;
}
bool GpuProcessingPhasePolicy::retireDeviceReport(const std::uint64_t ownershipEpoch,
                                                  const bool retirementProven) noexcept {
    if (!retirementProven) {
        return false;
    }
    const std::lock_guard lock(mutex_);
    for (std::size_t index = 0; index < deviceCount_; ++index) {
        if (devices_[index].ownershipEpoch == ownershipEpoch) {
            devices_[index] = devices_[deviceCount_ - 1];
            devices_[--deviceCount_] = {};
            applyDeviceReportsLocked();
            return true;
        }
    }
    return false;
}
void GpuProcessingPhasePolicy::applyDeviceReportsLocked() noexcept {
    std::optional<std::uint64_t> minimum;
    bool ready = deviceCount_ != 0;
    for (std::size_t slot = 0; slot < deviceCount_; ++slot) {
        const auto& current = devices_[slot].budget;
        if (current.source == render::GpuAllocationBudgetSource::Unavailable) {
            ready = false;
            continue;
        }
        // The fallback nominal heap estimate is a static planning quota, never free VRAM.
        // Reserve half for opaque driver/native overlap before applying per-request shares.
        const auto available =
            current.source == render::GpuAllocationBudgetSource::DeviceLocalEstimate
                ? current.available_bytes / 2
                : current.available_bytes;
        minimum = minimum ? std::min(*minimum, available) : available;
    }
    budget_.setDeviceCapacity(minimum, ready || (minimum && *minimum == 0));
}
render::GpuProcessingPhaseGrant
GpuProcessingPhasePolicy::reserve(const GpuProcessingPhaseEnvelope& envelope,
                                  const Clock::time_point now) noexcept {
    const auto request = processingPhaseRequest(envelope);
    if (!request) {
        return {};
    }
    const std::lock_guard lock(mutex_);
    if (!hostSampleAt_ || now < *hostSampleAt_ || now - *hostSampleAt_ > std::chrono::seconds(10)) {
        budget_.setHostCapacity(std::nullopt, false);
    }
    return budget_.reserve(*request);
}
GpuProcessingPhasePolicySnapshot GpuProcessingPhasePolicy::snapshot() const noexcept {
    const std::lock_guard lock(mutex_);
    bool nominal = false;
    for (std::size_t index = 0; index < deviceCount_; ++index) {
        nominal = nominal || devices_[index].budget.source ==
                                 render::GpuAllocationBudgetSource::DeviceLocalEstimate;
    }
    return {.reservations = budget_.snapshot(),
            .processSource = processSource_,
            .processStatus = processStatus_,
            .sampleSerial = sampleSerial_,
            .deviceReports = deviceCount_,
            .usesNominalDeviceEstimate = nominal};
}

render::GpuProcessingPhaseGrant reserveHostEvaluationPhase(GpuProcessingPhasePolicy& policy,
                                                           const std::uint64_t pixels,
                                                           const std::uint64_t requestedPixelBytes,
                                                           const GpuProcessingPhaseOutput output,
                                                           const bool encodedOutput) noexcept {
    if (pixels == 0 || pixels > std::numeric_limits<std::uint64_t>::max() / 16) {
        return {};
    }
    return policy.reserve({.output = output,
                           .pixels = pixels,
                           .encodedBytes = encodedOutput ? pixels * 16 : 0,
                           .metadataBytes = 16ULL * 1024 * 1024,
                           .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                           .requestedSourceBytes = requestedPixelBytes,
                           .minimumSourceBytes = pixels * 16,
                           .native = false});
}
} // namespace bloom::runtime
