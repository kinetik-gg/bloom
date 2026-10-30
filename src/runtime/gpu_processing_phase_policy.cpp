#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

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
std::optional<std::uint64_t> minusHeadroom(const std::optional<std::uint64_t> bytes,
                                           const std::uint64_t headroom) noexcept {
    if (!bytes)
        return bytes;
    return *bytes > headroom ? *bytes - headroom : 0;
}
std::uint64_t subtractSaturating(const std::uint64_t left, const std::uint64_t right) noexcept {
    return left > right ? left - right : 0;
}
const char* admissionName(const render::GpuProcessingPhaseAdmission status) noexcept {
    switch (status) {
    case render::GpuProcessingPhaseAdmission::Accepted:
        return "Accepted";
    case render::GpuProcessingPhaseAdmission::AwaitingCapacity:
        return "AwaitingCapacity";
    case render::GpuProcessingPhaseAdmission::BudgetRefused:
        return "BudgetRefused";
    case render::GpuProcessingPhaseAdmission::InvalidRequest:
        return "InvalidRequest";
    case render::GpuProcessingPhaseAdmission::AllocationFailure:
        return "AllocationFailure";
    case render::GpuProcessingPhaseAdmission::SerialExhausted:
        return "SerialExhausted";
    }
    return "Unknown";
}
const char* domainName(const render::GpuProcessingPhaseDomain domain) noexcept {
    switch (domain) {
    case render::GpuProcessingPhaseDomain::None:
        return "None";
    case render::GpuProcessingPhaseDomain::Host:
        return "Host";
    case render::GpuProcessingPhaseDomain::Device:
        return "Device";
    case render::GpuProcessingPhaseDomain::RequestCount:
        return "RequestCount";
    case render::GpuProcessingPhaseDomain::Viewer:
        return "Viewer";
    }
    return "Unknown";
}
std::optional<std::uint64_t> processHeadroom(const ProcessMemorySample& sample) noexcept {
    std::optional<std::uint64_t> result;
    for (std::size_t index = 0; index < sample.domains.size(); ++index) {
        const auto& domain = sample.domains[index];
        if (domain.kind != ProcessMemoryLimitKind::Finite)
            continue;
        // Clean page cache in the domain is reclaimable on demand and is not in use.
        const auto current = processMemoryDomainUnreclaimableBytes(sample, index);
        const auto remaining = domain.limitBytes > current ? domain.limitBytes - current : 0;
        result = result ? std::min<std::uint64_t>(*result, remaining) : remaining;
    }
    return result;
}
} // namespace
std::optional<render::GpuProcessingPhaseRequest>
processingPhaseRequest(const GpuProcessingPhaseEnvelope& envelope) noexcept {
    if (envelope.pixels == 0) {
        // A non-native, source-free output stage owns no pixel operation. It still needs the
        // existing root's request slot, and any explicit metadata must be admitted there.
        if (!envelope.sourceFree || envelope.native || envelope.encodedBytes != 0 ||
            envelope.retainedCoverageByteLimit != 0 || envelope.requestedSourceBytes != 0 ||
            envelope.minimumSourceBytes != 0 || envelope.requestedExecutorBytes.value_or(0) != 0 ||
            envelope.minimumExecutorBytes != 0 || envelope.deviceWorkingBytes != 0)
            return {};
        return render::GpuProcessingPhaseRequest{
            .fixedHostBytes = envelope.metadataBytes, .sourceFree = true, .native = false};
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
    // Native processing scratch and output allocator headers have separate live owners. Reserve
    // one metadata allowance for each so later scratch can transfer an existing phase claim even
    // when the source estimate consumes all remaining measured headroom.
    const auto scratchBytes = envelope.native ? envelope.metadataBytes : 0;
    auto fixed = add(*outputs, envelope.metadataBytes);
    if (fixed)
        fixed = add(*fixed, scratchBytes);
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
                                             .processingHostScratchBytes = scratchBytes,
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
            pendingHostBackingCredit_ = HostSampleBackingCredit{
                sample.process.sampledAt, credit, std::nullopt, 0, after.retiredMaterializedBytes};
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
            const auto compensated =
                subtractSaturating(addSaturating(*finite, duplicateCredit), headroom_.ramBytes);
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
    std::uint64_t backingCredit = 0;
    std::uint64_t retiredBytesAtSample = 0;
    if (fresh && pendingHostBackingCredit_.has_value()) {
        const auto& pending = *pendingHostBackingCredit_;
        if (pending.sampledAt == sample.process.sampledAt) {
            backingCredit = pending.bytes;
            if (backingCredit != 0)
                retiredBytesAtSample = pending.retiredBytesAtSample;
        }
    }
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
        allowance =
            current
                ? std::optional{std::min(*allowance, addSaturating(*current, headroom_.ramBytes))}
                : std::nullopt;
    }
    // The ceiling decides refusal versus waiting: a need within it waits for memory that other
    // work in the same limit releases; only a need above it can never be given.
    if (fresh) {
        std::optional<std::uint64_t> ceiling;
        if (physicalBytes != 0)
            ceiling = physicalBytes;
        for (const auto& domain : sample.process.domains)
            if (domain.kind == ProcessMemoryLimitKind::Finite)
                ceiling = ceiling ? std::min<std::uint64_t>(*ceiling, domain.limitBytes)
                                  : domain.limitBytes;
        hostCeiling_ = ceiling;
        budget_.setHostCeiling(minusHeadroom(hostCeiling_, headroom_.ramBytes));
    }
    // The RAM headroom preference is the only margin kept free of Bloom's work and caches.
    allowance = minusHeadroom(allowance, headroom_.ramBytes);
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
    // Every owner sharing this root reports the same physical GPU (one driver device UUID): the
    // live budget is process-wide there, so it already excludes all of the root's backing. One
    // owner is its own device; several need a known, identical UUID (equal heap sizes are not
    // proof: two identical cards report the same size).
    constexpr std::array<std::uint8_t, 16> unknownDevice{};
    bool sameLiveDevice = deviceCount_ != 0;
    for (std::size_t slot = 0; slot < deviceCount_; ++slot) {
        const auto& budget = devices_[slot].budget;
        sameLiveDevice =
            sameLiveDevice && budget.source == render::GpuAllocationBudgetSource::MemoryBudget &&
            (deviceCount_ == 1 || (budget.device_uuid != unknownDevice &&
                                   budget.device_uuid == devices_[0].budget.device_uuid));
    }
    if (sameLiveDevice) {
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
        // Without VK_EXT_memory_budget the device-local heap size is the documented capability
        // fallback; the root's own claims are subtracted by admission, the headroom below covers
        // other processes and driver overhead, and allocation failure stays a typed refusal.
        auto available = current.available_bytes;
        if (ownedLocalNative != 0) {
            const auto physicalRoom =
                current.device_local_bytes > available ? current.device_local_bytes - available : 0;
            available += std::min(ownedLocalNative, physicalRoom);
        }
        available = subtractSaturating(available, headroom_.vramBytes);
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
    render::GpuProcessingPhaseGrant grant;
    {
        const std::lock_guard lock(mutex_);
        if (!hostSampleAt_ || now < *hostSampleAt_ ||
            now - *hostSampleAt_ > std::chrono::seconds(10)) {
            budget_.setHostCapacity(std::nullopt, false);
        }
        const auto hostNeed = request->fixedHostBytes + 3 * request->minimumSourceBytes;
        const auto deviceNeed =
            request->native ? request->deviceWorkingBytes + request->minimumExecutorBytes : 0;
        const bool viewer = envelope.admissionClass == GpuAdmissionClass::Viewer;
        bool yield = false;
        if (!viewer) {
            const bool waiting = viewerWaitingAt_ && now >= *viewerWaitingAt_ &&
                                 now - *viewerWaitingAt_ <= kViewerActivityWindow;
            const bool active = viewerAdmittedAt_ && now >= *viewerAdmittedAt_ &&
                                now - *viewerAdmittedAt_ <= kViewerActivityWindow;
            if (waiting) {
                yield = true;
            } else if (active) {
                // Spare capacity only: leave the last Viewer need free on both domains.
                const auto counters = budget_.snapshot();
                const auto hostFree = counters.hostCapacityBytes
                                          ? subtractSaturating(*counters.hostCapacityBytes,
                                                               counters.totalHostClaimBytes)
                                          : 0;
                const auto deviceFree = counters.deviceCapacityBytes
                                            ? subtractSaturating(*counters.deviceCapacityBytes,
                                                                 counters.totalDeviceClaimBytes)
                                            : 0;
                yield = addSaturating(hostNeed, viewerNeedHostBytes_) > hostFree ||
                        (request->native &&
                         addSaturating(deviceNeed, viewerNeedDeviceBytes_) > deviceFree);
                // A frame that cannot fit beside the Viewer even when idle still runs: it only
                // waits while the Viewer is busy, never forever (the activity window lapses).
            }
        }
        if (yield) {
            grant = {.status = render::GpuProcessingPhaseAdmission::AwaitingCapacity,
                     .domain = render::GpuProcessingPhaseDomain::Viewer,
                     .lease = {},
                     .admissionRevision = budget_.snapshot().admissionRevision};
        } else {
            grant = budget_.reserve(*request);
        }
        if (viewer) {
            if (grant.status == render::GpuProcessingPhaseAdmission::Accepted) {
                viewerWaitingAt_.reset();
                viewerAdmittedAt_ = now;
                viewerNeedHostBytes_ = hostNeed;
                viewerNeedDeviceBytes_ = deviceNeed;
            } else if (grant.status == render::GpuProcessingPhaseAdmission::AwaitingCapacity) {
                viewerWaitingAt_ = now;
            }
        }
    }
    if (grant.status == render::GpuProcessingPhaseAdmission::AwaitingCapacity &&
        grant.domain != render::GpuProcessingPhaseDomain::RequestCount) {
        // Evict reusable retention first; the waiting request retries on the release.
        const auto needed = grant.domain == render::GpuProcessingPhaseDomain::Device
                                ? request->deviceWorkingBytes + request->minimumExecutorBytes
                                : request->fixedHostBytes + 3 * request->minimumSourceBytes;
        std::vector<RetentionReclaimer> reclaimers;
        {
            const std::lock_guard lock(reclaimerMutex_);
            reclaimers = reclaimers_;
        }
        for (const auto& reclaimer : reclaimers) {
            try {
                if (reclaimer(grant.domain, needed) >= needed)
                    break;
            } catch (...) {
            }
        }
    }
    if (grant.status != render::GpuProcessingPhaseAdmission::Accepted)
        logWaitOrRefusal(grant);
    return grant;
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

void GpuProcessingPhasePolicy::setHeadroom(const MemoryHeadroom headroom) noexcept {
    const std::lock_guard lock(mutex_);
    const auto previous = headroom_;
    headroom_ = headroom;
    budget_.shiftHostCapacity(previous.ramBytes, headroom.ramBytes);
    budget_.setHostCeiling(minusHeadroom(hostCeiling_, headroom_.ramBytes));
    if (deviceCount_ != 0)
        applyDeviceReportsLocked();
}
GpuProcessingPhasePolicy::ViewerPriority
GpuProcessingPhasePolicy::viewerPriority(const Clock::time_point now) const noexcept {
    const std::lock_guard lock(mutex_);
    return {.waiting = viewerWaitingAt_ && now >= *viewerWaitingAt_ &&
                       now - *viewerWaitingAt_ <= kViewerActivityWindow,
            .active = viewerAdmittedAt_ && now >= *viewerAdmittedAt_ &&
                      now - *viewerAdmittedAt_ <= kViewerActivityWindow,
            .reserveHostBytes = viewerNeedHostBytes_,
            .reserveDeviceBytes = viewerNeedDeviceBytes_};
}
UsableCapacity GpuProcessingPhasePolicy::usableCapacity() const noexcept {
    const auto counters = budget_.snapshot();
    UsableCapacity result;
    result.hostReady = counters.hostReady && counters.hostCapacityBytes.has_value();
    result.deviceReady = counters.deviceReady && counters.deviceCapacityBytes.has_value();
    result.hostUsableBytes = counters.hostCapacityBytes.value_or(0);
    result.deviceUsableBytes = counters.deviceCapacityBytes.value_or(0);
    result.hostClaimBytes = counters.totalHostClaimBytes;
    result.deviceClaimBytes = counters.totalDeviceClaimBytes;
    result.hostFreeBytes = subtractSaturating(result.hostUsableBytes, result.hostClaimBytes);
    result.deviceFreeBytes = subtractSaturating(result.deviceUsableBytes, result.deviceClaimBytes);
    result.hostRetentionBytes =
        addSaturating(counters.ownedPackedHostBytes, counters.ownedPackedHostMetadataBytes);
    result.admittedPhases = counters.admittedPhases;
    result.admissionRevision = counters.admissionRevision;
    return result;
}
std::size_t
GpuProcessingPhasePolicy::framesThatFit(const std::uint64_t hostBytesPerFrame,
                                        const std::uint64_t deviceBytesPerFrame) const noexcept {
    const auto capacity = usableCapacity();
    if (!capacity.hostReady || (deviceBytesPerFrame != 0 && !capacity.deviceReady))
        return 0;
    std::uint64_t frames = std::numeric_limits<std::uint64_t>::max();
    if (hostBytesPerFrame != 0)
        frames = std::min(frames, capacity.hostFreeBytes / hostBytesPerFrame);
    if (deviceBytesPerFrame != 0)
        frames = std::min(frames, capacity.deviceFreeBytes / deviceBytesPerFrame);
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, std::numeric_limits<std::size_t>::max()));
}
bool GpuProcessingPhasePolicy::waitForAdmissionChange(
    const std::uint64_t seenRevision, const CancellationToken& cancellation,
    const std::chrono::milliseconds maxWait) const {
    const auto deadline = std::chrono::steady_clock::now() + maxWait;
    const auto serial = snapshot().sampleSerial;
    while (!cancellation.isCancellationRequested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        // Short slices observe cancellation and new host samples (which may not move the
        // revision when the capacity is unchanged).
        if (budget_.waitForRevisionChange(seenRevision,
                                          std::min(deadline, now + std::chrono::milliseconds(50))))
            return true;
        if (snapshot().sampleSerial != serial)
            return true;
    }
    return false;
}
void GpuProcessingPhasePolicy::addRetentionReclaimer(RetentionReclaimer reclaimer) {
    if (!reclaimer)
        return;
    const std::lock_guard lock(reclaimerMutex_);
    reclaimers_.push_back(std::move(reclaimer));
}
void GpuProcessingPhasePolicy::logWaitOrRefusal(
    const render::GpuProcessingPhaseGrant& grant) noexcept {
    try {
        const auto now = std::chrono::steady_clock::now();
        {
            const std::lock_guard lock(reclaimerMutex_);
            // Waiting requests retry often; one line a second per root is enough to diagnose.
            if (grant.status == render::GpuProcessingPhaseAdmission::AwaitingCapacity &&
                now - lastAdmissionLog_ < std::chrono::seconds(1))
                return;
            lastAdmissionLog_ = now;
        }
        std::clog << "Bloom admission: " << describeAdmission(grant, usableCapacity()) << '\n';
    } catch (...) {
    }
}

std::string describeAdmission(const render::GpuProcessingPhaseGrant& grant,
                              const UsableCapacity& capacity) {
    std::ostringstream text;
    text << admissionName(grant.status) << " domain=" << domainName(grant.domain)
         << " hostUsable=" << capacity.hostUsableBytes << " hostClaims=" << capacity.hostClaimBytes
         << " hostRetention=" << capacity.hostRetentionBytes
         << " deviceUsable=" << capacity.deviceUsableBytes
         << " deviceClaims=" << capacity.deviceClaimBytes << " phases=" << capacity.admittedPhases
         << " hostReady=" << capacity.hostReady << " deviceReady=" << capacity.deviceReady
         << " revision=" << capacity.admissionRevision;
    return text.str();
}
MemoryHeadroom GpuProcessingPhasePolicy::headroom() const noexcept {
    const std::lock_guard lock(mutex_);
    return headroom_;
}

std::size_t previewFramesThatFit(const GpuProcessingPhasePolicy& policy,
                                 const std::uint64_t framePixels,
                                 const std::uint64_t deviceWorkingBytes) noexcept {
    constexpr std::uint64_t metadata = 64ULL * 1024 * 1024;
    if (framePixels == 0 ||
        framePixels > (std::numeric_limits<std::uint64_t>::max() - metadata) / 64)
        return 0;
    const auto host = framePixels * (8 + 3 * 16) + metadata;
    const auto device = addSaturating(deviceWorkingBytes, framePixels * 16);
    const auto threads = std::max(1U, std::thread::hardware_concurrency());
    return std::min<std::size_t>(policy.framesThatFit(host, device), threads);
}

render::GpuProcessingPhaseGrant
reserveHostEvaluationPhase(GpuProcessingPhasePolicy& policy, const std::uint64_t pixels,
                           const std::uint64_t requestedPixelBytes,
                           const GpuProcessingPhaseOutput output, const bool encodedOutput,
                           const GpuAdmissionClass admissionClass) noexcept {
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
                           .native = false,
                           .admissionClass = admissionClass});
}
} // namespace bloom::runtime
