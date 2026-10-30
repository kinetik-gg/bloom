#pragma once

#include <bloom/host/export_job_runner.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>

// Test support only (never linked into production): a deterministic, scriptable ExportRunBackend
// for ExportQueueService. Each poll() of a run advances it by exactly one step, so a test (or a
// src/ui panel test) can drive a job through Waiting -> Preparing -> Rendering with progress ->
// WaitingForViewer -> Done / Failed with a verification result / Stopped without evaluating or
// encoding anything. Destination samples are answered from an in-memory set, so a test can
// raise a DestinationExists NeedsConfirmation preflight without touching the filesystem.
namespace bloom::host::testing {

struct FakeExportScript final {
    enum class Outcome : std::uint8_t {
        Succeed,
        SucceedWithWarnings,
        // The output publishes but its verification fails (a ColourTags mismatch).
        FailVerification,
        // The output fails while rendering (EncoderFailed) on `failAtFrame`.
        FailRendering,
    };
    // Frames completed per poll while rendering.
    std::uint64_t framesPerPoll = 1;
    // After this many completed frames the route reports WaitingForViewer...
    std::uint64_t waitingForViewerAfterFrames = 0;
    // ...for this many polls (0: never).
    std::uint32_t waitingForViewerPolls = 0;
    Outcome outcome = Outcome::Succeed;
    std::uint64_t failAtFrame = 0;
};

class FakeExportRunBackend final : public ExportRunBackend {
  public:
    // The script the next started run uses (FIFO); runs without one succeed.
    void push(FakeExportScript script);
    // Destinations reported as existing (by first path), with a fingerprint derived from the
    // path and `generation`; bump the generation to simulate an external change.
    void setExisting(const std::filesystem::path& firstPath, std::uint64_t generation = 1);
    void clearExisting(const std::filesystem::path& firstPath);
    [[nodiscard]] std::size_t runsStarted() const;

    std::unique_ptr<ExportRun> start(ExportJobRunnerDependencies dependencies,
                                     ExportJobRunRequest request) override;
    ExportDestinationSample
    sampleDestination(const ExportDestinationSampleRequest& request) override;

  private:
    mutable std::mutex mutex_;
    std::deque<FakeExportScript> scripts_;
    std::map<std::filesystem::path, std::uint64_t> existing_;
    std::size_t started_ = 0;
};

} // namespace bloom::host::testing
