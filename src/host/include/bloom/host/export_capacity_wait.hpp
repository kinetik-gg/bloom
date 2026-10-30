#pragma once

#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

// The export side of capacity refusals: a typed wait at the frame boundary instead of a terminal
// failure. Shared by SequenceExportRunnerV1 and the export queue's image driver. Reads the shared
// processing root's counters; never reserves or changes admission.
namespace bloom::host {

// ADR 0033 section 9: during an export a capacity refusal means wait, not fail. AwaitingCapacity
// (capacity not sampled yet) always waits. BudgetRefused waits while other work holds admitted
// processing phases in the shared root (the Viewer, a RAM preview): they finish and release.
// `ownPhases` are the caller's own admitted phases (the movie frame queue's reservation), which
// never count as other work. With no other phase admitted the frame cannot fit now, and nothing
// in flight will change that, so it is NeverFits.
enum class OutputAnalysisCapacityRefusalV1 : std::uint8_t {
    // Not a capacity refusal.
    None,
    // Hold at the frame boundary and retry the same frame.
    Wait,
    // The frame does not fit even with nothing else reserved.
    NeverFits,
};
[[nodiscard]] OutputAnalysisCapacityRefusalV1
outputAnalysisCapacityRefusalV1(const OutputAnalysisAttemptOutcomeV1& outcome,
                                const runtime::GpuProcessingPhasePolicy* policy,
                                std::size_t ownPhases = 0) noexcept;
// True while the shared root admits more phases than the caller's own.
[[nodiscard]] bool otherProcessingPhasesAdmittedV1(const runtime::GpuProcessingPhasePolicy* policy,
                                                   std::size_t ownPhases) noexcept;
// The shared processing root's reservations (admitted phases, host and device claims against
// capacity) for the export log.
[[nodiscard]] std::string
describeProcessingReservationsV1(const runtime::GpuProcessingPhasePolicy* policy);

// The frame-boundary wait both export drivers use (SequenceExportRunnerV1 and the export queue's
// image driver): bounded exponential backoff between retries of the same frame, and a stall rule.
// Only an admitted processing phase finishes and releases capacity by itself; retained claims
// (caches, presentation) do not. A wait in which no phase was admitted anywhere for `stallAfter`
// therefore cannot end on its own and is reported as stalled, so the caller fails with readable
// text instead of waiting forever. Owner-thread only.
class OutputAnalysisCapacityWaitV1 final {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kFirstRetry{50};
    static constexpr std::chrono::milliseconds kLongestRetry{1000};
    static constexpr std::chrono::seconds kStallAfter{30};

    // A frame was refused for capacity at `now`; `policy` (nullable) is the shared processing
    // root the frame competes in.
    void refused(Clock::time_point now, const runtime::GpuProcessingPhasePolicy* policy,
                 std::size_t ownPhases = 0);
    // True while the frame waits for its next retry.
    [[nodiscard]] bool waiting() const noexcept { return waiting_; }
    // The backoff elapsed: retry the frame now (the wait stays open until succeeded()).
    [[nodiscard]] bool retryDue(Clock::time_point now) const noexcept;
    // The wait cannot end by itself (see the class comment).
    [[nodiscard]] bool stalled(Clock::time_point now,
                               const runtime::GpuProcessingPhasePolicy* policy,
                               std::size_t ownPhases = 0);
    // The owner paused while waiting: paused time never counts toward the stall, and the retry is
    // due as soon as the owner resumes.
    void paused(Clock::time_point now) noexcept {
        lastActivePhase_ = now;
        lastRefusal_ = now - backoff_;
    }
    // The frame was admitted; the next refusal starts a new wait.
    void succeeded() noexcept;
    // Total time spent waiting, for deadline accounting and logs.
    [[nodiscard]] Clock::duration waitedTotal() const noexcept { return waitedTotal_; }

  private:
    bool waiting_ = false;
    Clock::time_point started_{};
    Clock::time_point lastRefusal_{};
    Clock::time_point lastActivePhase_{};
    std::chrono::milliseconds backoff_{kFirstRetry};
    Clock::duration waitedTotal_{};
};

} // namespace bloom::host
