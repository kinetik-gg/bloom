#include <bloom/host/export_capacity_wait.hpp>

#include <algorithm>
#include <optional>
#include <string>

namespace bloom::host {

std::string describeProcessingReservationsV1(const runtime::GpuProcessingPhasePolicy* policy) {
    if (!policy)
        return "reservations=unknown (no shared processing root)";
    const auto held = policy->snapshot().reservations;
    const auto domain = [](const char* name, const bool ready,
                           const std::optional<std::uint64_t>& capacity,
                           const std::uint64_t reserved, const std::uint64_t claimed) {
        std::string text =
            std::string(name) + "{ready=" + (ready ? "1" : "0") +
            ", capacity=" + (capacity ? std::to_string(*capacity) : std::string("unsampled")) +
            ", reserved=" + std::to_string(reserved) + ", claimed=" + std::to_string(claimed);
        if (capacity)
            text += ", free=" + std::to_string(*capacity > claimed ? *capacity - claimed : 0);
        return text + "}";
    };
    return "admittedPhases=" + std::to_string(held.admittedPhases) + "/" +
           std::to_string(held.requestCapacity) + "; " +
           domain("host", held.hostReady, held.hostCapacityBytes, held.hostReservedBytes,
                  held.totalHostClaimBytes) +
           "; " +
           domain("device", held.deviceReady, held.deviceCapacityBytes, held.deviceReservedBytes,
                  held.totalDeviceClaimBytes) +
           "; budgetRefusals=" + std::to_string(held.budgetRefusals);
}

bool otherProcessingPhasesAdmittedV1(const runtime::GpuProcessingPhasePolicy* policy,
                                     const std::size_t ownPhases) noexcept {
    return policy != nullptr && policy->snapshot().reservations.admittedPhases > ownPhases;
}

OutputAnalysisCapacityRefusalV1
outputAnalysisCapacityRefusalV1(const OutputAnalysisAttemptOutcomeV1& outcome,
                                const runtime::GpuProcessingPhasePolicy* policy,
                                const std::size_t ownPhases) noexcept {
    const auto* failure = outcome.failure();
    if (!failure || failure->cancelled())
        return OutputAnalysisCapacityRefusalV1::None;
    if (const auto* code = failure->payloadAs<runtime::GpuProcessFrameDiagnosticCode>()) {
        if (*code == runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity)
            return OutputAnalysisCapacityRefusalV1::Wait;
        if (*code == runtime::GpuProcessFrameDiagnosticCode::BudgetRefused)
            return otherProcessingPhasesAdmittedV1(policy, ownPhases)
                       ? OutputAnalysisCapacityRefusalV1::Wait
                       : OutputAnalysisCapacityRefusalV1::NeverFits;
    }
    if (const auto* code = failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>();
        code && *code == output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed)
        return OutputAnalysisCapacityRefusalV1::Wait;
    return OutputAnalysisCapacityRefusalV1::None;
}

void OutputAnalysisCapacityWaitV1::refused(const Clock::time_point now,
                                           const runtime::GpuProcessingPhasePolicy* policy,
                                           const std::size_t ownPhases) {
    if (!waiting_) {
        waiting_ = true;
        started_ = now;
        lastActivePhase_ = now;
        backoff_ = kFirstRetry;
    } else {
        backoff_ = std::min(backoff_ * 2, kLongestRetry);
    }
    lastRefusal_ = now;
    if (otherProcessingPhasesAdmittedV1(policy, ownPhases))
        lastActivePhase_ = now;
}

bool OutputAnalysisCapacityWaitV1::retryDue(const Clock::time_point now) const noexcept {
    return waiting_ && now - lastRefusal_ >= backoff_;
}

bool OutputAnalysisCapacityWaitV1::stalled(const Clock::time_point now,
                                           const runtime::GpuProcessingPhasePolicy* policy,
                                           const std::size_t ownPhases) {
    if (!waiting_)
        return false;
    if (otherProcessingPhasesAdmittedV1(policy, ownPhases))
        lastActivePhase_ = now;
    return now - lastActivePhase_ >= kStallAfter;
}

void OutputAnalysisCapacityWaitV1::succeeded() noexcept {
    if (waiting_)
        waitedTotal_ += lastRefusal_ - started_;
    waiting_ = false;
    backoff_ = kFirstRetry;
}

} // namespace bloom::host
