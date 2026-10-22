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
std::uint64_t addSaturating(const std::uint64_t left, const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}
std::optional<std::uint64_t> multiply(const std::uint64_t value,
                                      const std::uint64_t factor) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() / factor) {
        return {};
    }
    return value * factor;
}
std::optional<std::uint64_t> processHeadroom(const ProcessMemorySample& sample) noexcept {
    std::optional<std::uint64_t> result;
    for (const auto& domain : sample.domains) {
        if (domain.kind != ProcessMemoryLimitKind::Finite)
            continue;
        const auto current = domain.currentBytes.value_or(0);
        const auto remaining = domain.limitBytes > current ? domain.limitBytes - current : 0;
        result = result ? std::min<std::uint64_t>(*result, remaining) : remaining;
    }
    return result;
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
MemoryBudgetSample
GpuProcessingPhasePolicy::sampleHostMemory(const CancellationToken& cancellation,
                                           const HostMemorySampler sampler) noexcept {
    const auto before = budget_.rootedHostBackingSnapshot();
    auto sample = sampler ? sampler(cancellation) : MemoryBudgetSample{};
    const auto after = budget_.rootedHostBackingSnapshot();
    // Subtract every materialized retirement during the measurement, including newly allocated
    // backing that retires in that interval. That can under-credit but cannot over-credit.
    std::uint64_t credit = 0;
    if (!before.retiredBytesSaturated && !after.retiredBytesSaturated &&
        after.retiredMaterializedBytes >= before.retiredMaterializedBytes) {
        const auto retired = after.retiredMaterializedBytes - before.retiredMaterializedBytes;
        credit = before.materializedBytes > retired ? before.materializedBytes - retired : 0;
    }
    if (!cancellation.isCancellationRequested()) {
        const std::lock_guard lock(mutex_);
        if (!pendingHostBackingCredit_ ||
            sample.process.sampledAt >= pendingHostBackingCredit_->sampledAt)
            pendingHostBackingCredit_ =
                HostSampleBackingCredit{sample.process.sampledAt, credit, std::nullopt, 0,
                                        after.retiredMaterializedBytes};
    }
    return sample;
}
void GpuProcessingPhasePolicy::applyHostSample(const MemoryBudgetState& sample,
                                               const std::size_t physicalBytes,
                                               const Clock::time_point now) noexcept {
    const std::lock_guard lock(mutex_);
    const bool newerDelivery = sample.sampleSerial > sampleSerial_;
    const auto finite = processHeadroom(sample.process);
    const auto duplicateCredit =
        appliedHostBackingCredit_ &&
                appliedHostBackingCredit_->sampledAt == sample.process.sampledAt &&
                appliedHostBackingCredit_->finiteHeadroom == finite &&
                appliedHostBackingCredit_->sampleSerial == sample.sampleSerial
            ? appliedHostBackingCredit_->bytes
            : 0;
    if (newerDelivery) {
        processSource_ = sample.process.source;
        processStatus_ = sample.process.status;
        sampleSerial_ = sample.sampleSerial;
    }
    if (!newerDelivery) {
        // Multiple composition roots may replay the same already-delivered neutral snapshot.
        // A duplicate never closes/reopens readiness or authorizes growth; reliable tighter
        // constraints remain admissible even on this path.
        processConstraintHistory_.recordFinite(sample.process, false);
        if (finite) {
            finiteProcessCeiling_ = finiteProcessCeiling_
                                        ? std::min<std::uint64_t>(*finiteProcessCeiling_, *finite)
                                        : *finite;
            const auto compensated = addSaturating(*finite, duplicateCredit);
            budget_.tightenHostCapacity(compensated, false,
                                       hostSampleAt_.value_or(Clock::time_point::min()) +
                                           std::chrono::seconds(10));
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
    const auto backingCredit =
        fresh && pendingHostBackingCredit_ &&
                pendingHostBackingCredit_->sampledAt == sample.process.sampledAt
            ? pendingHostBackingCredit_->bytes
            : 0;
    const auto retiredBytesAtSample =
        backingCredit != 0 ? pendingHostBackingCredit_->retiredBytesAtSample : 0;
    if (fresh) {
        appliedHostBackingCredit_ =
            HostSampleBackingCredit{sample.process.sampledAt, backingCredit, finite,
                                    sample.sampleSerial, retiredBytesAtSample};
        pendingHostBackingCredit_.reset();
    }
    processConstraintHistory_.recordFinite(
        sample.process, complete && fresh && sample.process.status == ProcessMemoryStatus::Finite);
    if (complete && fresh && processConstraintHistory_.verifiesRemoval(sample.process)) {
        finiteProcessCeiling_.reset();
    }
    if (fresh && sample.process.status == ProcessMemoryStatus::Finite &&
        isCompleteProcessMemorySample(sample.process) && finite) {
        finiteProcessCeiling_ = finite;
    }
    // A retained finite ceiling was measured at an older time. Backing created since that
    // observation was not included in its free-space reading and must not be added to it.
    const auto finiteBackingCredit =
        fresh && sample.process.status == ProcessMemoryStatus::Finite &&
                isCompleteProcessMemorySample(sample.process) && finite
            ? backingCredit
            : 0;
    std::optional<std::uint64_t> allowance;
    if (sample.machine.availableBytes) {
        // Measured headroom bounds new work. Cache preferences and arbitrary reserve percentages
        // do not limit whether a frame can render; typed phase claims still bound its live peak.
        allowance.emplace(addSaturating(*sample.machine.availableBytes, backingCredit));
        if (physicalBytes != 0)
            allowance.emplace(std::min<std::uint64_t>(*allowance, physicalBytes));
        if (finiteProcessCeiling_) {
            allowance.emplace(
                std::min(*allowance, addSaturating(*finiteProcessCeiling_, finiteBackingCredit)));
        }
    } else if (finiteProcessCeiling_ && *finiteProcessCeiling_ == 0) {
        allowance.emplace(0);
    }
    if (allowance && *allowance != 0 && fresh && (!complete || sample.awaitingProcessSample)) {
        const auto current = budget_.snapshot().hostCapacityBytes;
        allowance = current ? std::optional{std::min(*allowance, *current)} : std::nullopt;
    }
    const bool ready =
        allowance && fresh && (*allowance == 0 || (complete && !sample.awaitingProcessSample));
    if (fresh) {
        hostSampleAt_ = sample.process.sampledAt;
    }
    const auto validUntil =
        hostSampleAt_.value_or(Clock::time_point::min()) + std::chrono::seconds(10);
    if (fresh)
        budget_.setSampledHostCapacity(allowance, ready, validUntil, backingCredit,
                                       retiredBytesAtSample);
    else
        budget_.tightenHostCapacity(allowance, true, validUntil);
}
void GpuProcessingPhasePolicy::applyHostSample(const MemoryBudgetState& sample,
                                               const std::size_t physicalBytes,
                                               const MemoryBudgetAllocation& allocation,
                                               const Clock::time_point now) noexcept {
    const bool newerDelivery = sample.sampleSerial > snapshot().sampleSerial;
    applyHostSample(sample, physicalBytes, now);
    if (!budgetView_ || !newerDelivery)
        return;
    const auto current = snapshot();
    if (current.sampleSerial != sample.sampleSerial)
        return;
    const auto previous = budgetView_->load();
    if (previous && previous->sampleSerial >= current.sampleSerial)
        return;
    const auto host = current.reservations.hostCapacityBytes.value_or(0);
    const auto hostCeiling = static_cast<std::size_t>(
        std::min<std::uint64_t>(host, std::numeric_limits<std::size_t>::max()));
    // Retention targets are descriptive only. Small caches must not prohibit foreground work.
    // The shared phase registry and real device quotas enforce concurrent allocation claims.
    static_cast<void>(
        budgetView_->publish({.sampleSerial = current.sampleSerial,
                              .sampledAt = sample.process.sampledAt,
                              .ready = current.reservations.hostReady &&
                                       sample.processSampleFresh && !sample.awaitingProcessSample,
                              .assignedPreviewBytes = allocation.previewFrameCacheByteBudget,
                              .assignedOperationBytes = allocation.operationCacheByteBudget,
                              .previewRequestBytes = hostCeiling,
                              .aggregateGpuBytes = hostCeiling}));
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
    // With one reported device, the root's exact LOCAL native allocations belong to that owner.
    // A live VMA free-space report has already subtracted their backing, while totalDeviceClaim
    // still includes their ownership tokens. Credit only this allocated portion before the phase
    // budget subtracts all claims; unallocated phase reservations remain charged. With several
    // devices the aggregate root has no per-device ownership split, so use raw free conservatively.
    std::uint64_t ownedLocalNative = 0;
    if (deviceCount_ == 1 &&
        devices_[0].budget.source == render::GpuAllocationBudgetSource::MemoryBudget) {
        const auto claims = budget_.snapshot();
        const auto localImages =
            claims.ownedNativeImageBytes -
            std::min(claims.ownedNativeImageBytes, claims.ownedNonlocalNativeHostBytes);
        const auto localBuffers =
            claims.ownedNativeBufferBytes -
            std::min(claims.ownedNativeBufferBytes, claims.ownedNonlocalNativeBufferHostBytes);
        ownedLocalNative = localImages > std::numeric_limits<std::uint64_t>::max() - localBuffers
                               ? std::numeric_limits<std::uint64_t>::max()
                               : localImages + localBuffers;
    }
    for (std::size_t slot = 0; slot < deviceCount_; ++slot) {
        const auto& current = devices_[slot].budget;
        if (current.source == render::GpuAllocationBudgetSource::Unavailable) {
            ready = false;
            continue;
        }
        // The fallback nominal heap estimate is a static planning quota, never free VRAM.
        // Reserve half for opaque driver/native overlap before applying per-request shares.
        auto available = current.source == render::GpuAllocationBudgetSource::DeviceLocalEstimate
                             ? current.available_bytes / 2
                             : current.available_bytes;
        if (ownedLocalNative != 0) {
            const auto physicalRoom =
                current.device_local_bytes > available ? current.device_local_bytes - available : 0;
            available += std::min(ownedLocalNative, physicalRoom);
        }
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
