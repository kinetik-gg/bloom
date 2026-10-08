#pragma once

#include <bloom/runtime/prepared_preview_frame.hpp>

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace bloom::ui {

enum class PreviewTraceStage : std::uint8_t {
    Desired,
    Queued,
    Delivered,
    Published,
    CacheLookup,
    CacheRetention,
    CacheRemoval,
    PresentSubmitted,
    PresentAcknowledged,
    CpuPaint
};
enum class PreviewTraceOutcome : std::uint8_t {
    Requested,
    Accepted,
    Rejected,
    Current,
    Stale,
    Cancelled,
    Failed,
    Unsupported,
    Hit,
    Miss,
    Retained,
    Reused,
    Removed,
    Late,
    Invalid,
    OutOfRange,
    WrongRevision,
    OverBudget,
    AllocationFailure
};
enum class PreviewTracePhase : std::uint8_t { Unclassified, Cold, Warm, Manipulation };

// Numeric-only, UI-owned diagnostic stream. This is a request-intent fingerprint, NOT the
// canonical process/display cache key or a cryptographic digest. No locators, text, labels,
// diagnostics or display/view strings are retained. Exact publication correlation uses the
// project/composition/revision/generation tuple; the fingerprint additionally describes intent.
struct PreviewTraceIdentity final {
    std::uint64_t project = 0, composition = 0, revision = 0, generation = 0, intent = 0;
    std::int64_t timeNumerator = 0, timeDenominator = 1;
    friend bool operator==(const PreviewTraceIdentity&, const PreviewTraceIdentity&) = default;
};
struct PreviewTraceEvent final {
    std::uint64_t ordinal = 0;
    std::int64_t wallNanoseconds = 0; // steady-clock CPU wall time since recorder creation
    PreviewTraceIdentity identity;
    PreviewTraceStage stage = PreviewTraceStage::Desired;
    PreviewTraceOutcome outcome = PreviewTraceOutcome::Requested;
    // 255 means unknown/unavailable, never an inferred CPU/GPU provider.
    std::uint8_t provider = 255;
    PreviewTracePhase phase = PreviewTracePhase::Unclassified;
    std::uint64_t task = 0, sequence = 0, appliedSequence = 0, nativePresentCount = 0,
                  presenter = 0;
};

class PreviewFrameTrace final {
  public:
    // Allocated once on opt-in. The oldest events are overwritten, counted explicitly.
    explicit PreviewFrameTrace(std::size_t capacity = 4096);
    [[nodiscard]] static PreviewTraceIdentity
    identity(const runtime::PreviewRequestIdentity&) noexcept;
    void record(PreviewTraceIdentity identity, PreviewTraceStage stage, PreviewTraceOutcome outcome,
                std::uint8_t provider = 255, std::uint64_t task = 0, std::uint64_t sequence = 0,
                std::uint64_t appliedSequence = 0, std::uint64_t nativePresentCount = 0,
                std::uint64_t presenter = 0) noexcept;
    [[nodiscard]] std::uint64_t allocatePresenterId() noexcept { return ++presenterId_; }
    void record(const runtime::PreviewRequestIdentity& identity, PreviewTraceStage stage,
                PreviewTraceOutcome outcome, std::uint8_t provider = 255,
                std::uint64_t task = 0) noexcept;
    void record(const runtime::PreparedPreviewFrame& frame, PreviewTraceStage stage,
                PreviewTraceOutcome outcome) noexcept;
    [[nodiscard]] std::vector<PreviewTraceEvent> events() const;
    [[nodiscard]] const std::optional<PreviewTraceEvent>&
    lastNativeAcknowledgement() const noexcept {
        return nativeAck_;
    }
    [[nodiscard]] std::uint64_t overwritten() const noexcept { return overwritten_; }
    void noteBridgePoll() noexcept { ++bridgePolls_; }
    void noteForegroundPoll() noexcept { ++foregroundPolls_; }
    void setPhase(PreviewTracePhase phase) noexcept { phase_ = phase; }
    void setPollingIntervals(std::chrono::milliseconds bridge,
                             std::chrono::milliseconds foreground) noexcept;
    // Call on a worker after UI recording has stopped. Formatting/I/O never happens in record().
    void writeJson(std::ostream& output) const;

  private:
    std::vector<PreviewTraceEvent> buffer_;
    std::size_t next_ = 0, size_ = 0;
    std::uint64_t ordinal_ = 0, overwritten_ = 0, presenterId_ = 0;
    std::uint64_t bridgePolls_ = 0, foregroundPolls_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::milliseconds bridgeInterval_{0}, foregroundInterval_{0};
    std::optional<PreviewTraceEvent> nativeAck_;
    PreviewTracePhase phase_ = PreviewTracePhase::Unclassified;
};

} // namespace bloom::ui
