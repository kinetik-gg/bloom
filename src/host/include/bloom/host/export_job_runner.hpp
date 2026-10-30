#pragma once

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/export_verification.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/task_types.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::output {
class ExportResourceLedgerV1;
} // namespace bloom::output
namespace bloom::platform {
class StagedArtifactCoordinator;
} // namespace bloom::platform
namespace bloom::runtime {
class QualifiedDisplayProcessorProvider;
class SnapshotCompiler;
class TaskScheduler;
} // namespace bloom::runtime

// ADR 0033, section 6: the one runner every export route uses (the Export panel through
// ExportQueueService, scripting, MCP, bloom-cli and the old export dialog's shim). It renders one
// captured job: each enabled output is its own pass through the existing attempt -> policy
// approval -> publication path (images per frame through executeExportPublication, movies and WAV
// through SequenceExportRunnerV1), then the job-level verification.
//
// Owner-thread object: poll() never blocks and performs no I/O; all work is scheduler tasks owned
// by `owner`. The runner captures everything at construction, so edits made after Start never
// change a running job.
namespace bloom::host {

class GpuExportProvider;
class PublicationCoordinator;
class SequenceExportRetirementOwnerV1;

struct ExportJobRunnerDependencies final {
    runtime::TaskScheduler& scheduler;
    const runtime::SnapshotCompiler& compiler;
    PublicationCoordinator& publications;
    platform::StagedArtifactCoordinator& artifacts;
    std::shared_ptr<output::ExportResourceLedgerV1> ledger;
    std::shared_ptr<GpuExportProvider> gpu;
    std::shared_ptr<SequenceExportRetirementOwnerV1> retirement;
    // Optional application-wide qualified display processor (the Viewer's); PNG reuses it.
    runtime::QualifiedDisplayProcessorProvider* displayProcessor = nullptr;
    // Caller-owned scratch folder for publication's bridging files.
    std::filesystem::path scratchDirectory;
};

// One output of the captured job, resolved at Preparing.
struct ExportRunOutput final {
    document::ExportOutput record;
    // The context the destination was resolved against, frozen when the run started.
    ExportDestinationContext context;
    ExportResolvedDestination destination;
    // The confirmed destination sample (fingerprint digest of the existing file or file set). The
    // run re-samples and refuses with PublicationConflict when it changed. Absent when nothing
    // existed or the policy is Replace.
    std::optional<core::Sha256Digest> confirmedSample;
    // Refuse when the destination exists at run time (policy Refuse).
    bool refuseExisting = false;
    // Issue #369: the destination's sampled free space minus the safety margin.
    std::optional<std::uint64_t> artifactByteLimit;
};

struct ExportJobRunRequest final {
    document::Snapshot snapshot;
    document::ExportJob job;
    std::string compositionName;
    ExportResolvedRange range;
    document::ColorSettings colorSettings;
    // The project folder (relative asset locators resolve against it); empty when unsaved.
    std::filesystem::path assetBaseDirectory;
    // Enabled outputs in job order. Disabled outputs are the caller's Skipped rows.
    std::vector<ExportRunOutput> outputs;
    ExportEncoderStatus encoders;
    runtime::TaskOwner owner{runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)};
    std::optional<runtime::TaskGroupId> group;
};

enum class ExportJobRunPhase : std::uint8_t {
    Preparing = 1,
    Rendering = 2,
    Verifying = 3,
    Finished = 4,
};

struct ExportOutputRunState final {
    document::ExportOutputId output;
    ExportOutputState state = ExportOutputState::Pending;
    std::optional<ExportOutputVerification> verification;
    std::optional<ExportFailure> failure;
    // Paths this run published (a movie's file, or a sequence's first and last frames).
    std::vector<std::filesystem::path> published;
};

// The runner seam ExportQueueService drives: one captured run. The production implementation is
// ExportJobRunner; tests supply a deterministic fake (src/host/testing). Owner-thread object;
// every member is non-blocking.
class ExportRun {
  public:
    ExportRun() = default;
    ExportRun(const ExportRun&) = delete;
    ExportRun& operator=(const ExportRun&) = delete;
    virtual ~ExportRun() = default;

    // Advances finished tasks.
    virtual void poll() = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
    // True once a pause request reached a frame boundary.
    [[nodiscard]] virtual bool paused() const noexcept = 0;
    [[nodiscard]] virtual bool pauseRequested() const noexcept = 0;

    [[nodiscard]] virtual ExportJobRunPhase phase() const noexcept = 0;
    [[nodiscard]] bool finished() const noexcept { return phase() == ExportJobRunPhase::Finished; }
    [[nodiscard]] virtual bool stopped() const noexcept = 0;
    [[nodiscard]] virtual const std::optional<ExportFailure>& failure() const noexcept = 0;
    [[nodiscard]] virtual std::span<const ExportOutputRunState> outputs() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t framesCompleted() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t framesTotal() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> currentFrame() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t outputIndex() const noexcept = 0;
    [[nodiscard]] virtual ExportDeviceRoute route() const noexcept = 0;
    // While route() is WaitingForMemory: the bytes the waiting frame needs.
    [[nodiscard]] virtual std::optional<std::uint64_t> memoryNeededBytes() const noexcept {
        return std::nullopt;
    }
    // Log entries produced since the last call (code, severity, output, frame, message).
    [[nodiscard]] virtual std::vector<ExportLogEntry> takeLog() = 0;
};

// One worker sample of an output's destination (ADR 0033 section 7).
struct ExportDestinationSampleRequest final {
    std::filesystem::path firstPath;
    std::filesystem::path lastPath;
    std::uint64_t fileCount = 0;
    // The authoritative re-sample at Preparing: also creates missing destination folders and
    // removes this target's stale staged files left by processes that are gone.
    bool forRun = false;
};

struct ExportDestinationSample final {
    std::filesystem::path firstPath;
    std::filesystem::path lastPath;
    std::uint64_t fileCount = 0;
    bool folderExists = false;
    // The folder, or its nearest existing ancestor, accepts new files.
    bool folderWritable = false;
    // Files this output would write that already exist.
    std::uint64_t existing = 0;
    // SHA-256 over the existing files' names, sizes and modification times.
    std::optional<core::Sha256Digest> fingerprint;
    std::optional<std::uint64_t> freeBytes;
};

// What the service uses to run jobs and sample destinations.
class ExportRunBackend {
  public:
    ExportRunBackend() = default;
    ExportRunBackend(const ExportRunBackend&) = delete;
    ExportRunBackend& operator=(const ExportRunBackend&) = delete;
    virtual ~ExportRunBackend() = default;
    // Owner thread, non-blocking: starts one captured run.
    [[nodiscard]] virtual std::unique_ptr<ExportRun> start(ExportJobRunnerDependencies dependencies,
                                                           ExportJobRunRequest request) = 0;
    // Blocking-I/O worker thread only.
    [[nodiscard]] virtual ExportDestinationSample
    sampleDestination(const ExportDestinationSampleRequest& request) = 0;
};

// The production backend: ExportJobRunner and the platform destination facts.
[[nodiscard]] std::shared_ptr<ExportRunBackend> makeProductionExportRunBackend();

class ExportJobRunner final : public ExportRun {
  public:
    ExportJobRunner(ExportJobRunnerDependencies dependencies, ExportJobRunRequest request);
    ~ExportJobRunner() override;

    void poll() override;
    void stop() override;
    void setPaused(bool paused) override;
    [[nodiscard]] bool paused() const noexcept override;
    [[nodiscard]] bool pauseRequested() const noexcept override;
    [[nodiscard]] ExportJobRunPhase phase() const noexcept override;
    [[nodiscard]] bool stopped() const noexcept override;
    [[nodiscard]] const std::optional<ExportFailure>& failure() const noexcept override;
    [[nodiscard]] std::span<const ExportOutputRunState> outputs() const noexcept override;
    [[nodiscard]] std::uint64_t framesCompleted() const noexcept override;
    [[nodiscard]] std::uint64_t framesTotal() const noexcept override;
    [[nodiscard]] std::optional<std::uint64_t> currentFrame() const noexcept override;
    [[nodiscard]] std::uint32_t outputIndex() const noexcept override;
    [[nodiscard]] ExportDeviceRoute route() const noexcept override;
    [[nodiscard]] std::optional<std::uint64_t> memoryNeededBytes() const noexcept override;
    [[nodiscard]] std::vector<ExportLogEntry> takeLog() override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// The evaluation colour intent of `composition` under `settings` (the working space, the selected
// OCIO config URI and pinned revision), captured so it owns its strings.
[[nodiscard]] std::optional<runtime::EvaluationColorIntent>
exportColorIntent(const document::ColorSettings& settings,
                  const document::Composition& composition);

// The frames a job renders now; nullopt when the composition is missing, has no frame mapping,
// or a custom/frame range lies outside it.
[[nodiscard]] std::optional<ExportResolvedRange>
resolveExportRange(const document::ExportJob& job, const document::Project& project);

} // namespace bloom::host
