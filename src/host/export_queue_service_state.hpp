#pragma once

// Private to src/host: ExportQueueService's runtime state (ADR 0033 sections 2, 4, 7, 8 and 11).
// Everything here is runtime state: it is never saved and never part of project truth.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/host/export_job_runner.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace bloom::host {

struct ExportQueueSubscription::State {
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::uint64_t serial = 0;
    bool closed = false;
};

namespace detail {

using DestinationSample = ExportDestinationSample;

// What a sample was taken for: the resolved destination and the settings that shape it.
struct SampleKey final {
    std::filesystem::path firstPath;
    std::filesystem::path lastPath;
    std::uint64_t fileCount = 0;
    friend bool operator==(const SampleKey&, const SampleKey&) = default;
};

struct OutputRuntime final {
    ExportOutputState state = ExportOutputState::Pending;
    std::optional<ExportOutputVerification> verification;
    ExportAssetImportResult import;
    std::vector<std::filesystem::path> published;
    // Import preparation in flight (one worker task) and its prepared operation.
    std::optional<runtime::TaskHandle<std::shared_ptr<commands::Operation>>> importTask;
    std::shared_ptr<commands::Operation> preparedImport;
    std::optional<document::AssetId> refreshAsset;
    std::string importFailure;
};

struct JobRuntime final {
    ExportJobState state = ExportJobState::Ready;
    std::uint64_t revision = 1;
    std::optional<ExportRunLink> run;
    std::optional<runtime::TaskGroupHandle> group;
    std::optional<ExportFailure> failure;
    std::optional<ExportStopCause> stopCause;
    std::optional<ExportStopCause> pendingStopCause;
    // The record captured at Preparing; the run keeps it whatever happens to the document.
    std::optional<document::ExportJob> captured;
    std::optional<ExportResolvedRange> capturedRange;
    std::optional<document::Snapshot> capturedSnapshot;
    std::string capturedCompositionName;
    std::uint16_t year = 1970;
    std::uint8_t month = 1;
    std::uint8_t day = 1;
    bool dateFrozen = false;
    // Preparing: the authoritative re-sample of every enabled output.
    std::map<ExportOutputId, runtime::TaskHandle<std::shared_ptr<const DestinationSample>>>
        preparingSamples;
    std::map<ExportOutputId, DestinationSample> preparedSamples;
    std::unique_ptr<ExportRun> runner;
    std::map<ExportOutputId, OutputRuntime> outputs;
    // Log (bounded per run).
    std::deque<ExportLogEntry> log;
    std::uint64_t logCount = 0;
    std::uint64_t lastLogSequence = 0;
    // Progress timing.
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::duration excluded{};
    std::optional<std::chrono::steady_clock::time_point> excludedSince;
    std::deque<std::chrono::steady_clock::time_point> frameTimes;
    std::uint64_t lastFrames = 0;
    ExportJobProgress progress;
    // Snapshot bookkeeping.
    std::string viewKey;
    bool transient = false;
    bool terminalEventPending = false;
    std::optional<ExportJobState> pendingTerminal;
};

struct PendingSample final {
    ExportOutputId output;
    SampleKey key;
    runtime::TaskHandle<std::shared_ptr<const DestinationSample>> handle;
};

} // namespace detail

struct ExportQueueService::State {
    explicit State(ExportQueueServiceDependencies value) : dependencies(std::move(value)) {
        if (!dependencies.backend)
            dependencies.backend = makeProductionExportRunBackend();
    }

    ExportQueueServiceDependencies dependencies;

    // Any-thread readers.
    mutable std::mutex mutex;
    std::shared_ptr<const ExportQueueSnapshot> published =
        std::make_shared<const ExportQueueSnapshot>();
    std::deque<ExportQueueEvent> events;
    std::uint64_t nextEventSequence = 1;
    std::map<ExportJobId, std::deque<ExportLogEntry>> logs;
    std::map<std::uint64_t, ExportJobId> owners;
    std::vector<std::shared_ptr<ExportQueueSubscription::State>> subscriptions;
    bool closed = false;

    // Owner thread only.
    std::uint64_t revision = 0;
    std::uint64_t nextRun = 1;
    std::uint64_t nextLogSequence = 1;
    std::optional<document::Snapshot> snapshot;
    std::optional<document::ColorSettings> colorSettings;
    std::optional<std::filesystem::path> projectFolder;
    bool editable = false;
    std::map<ExportJobId, detail::JobRuntime> jobs;
    std::deque<ExportJobId> waiting;
    std::optional<ExportJobId> active;
    ExportRunnerState runner = ExportRunnerState::Idle;
    std::optional<ExportStopCause> shutdownCause;
    ExportQueueDrained drained;
    std::map<ExportOutputId, detail::DestinationSample> samples;
    std::map<ExportOutputId, detail::SampleKey> sampleKeys;
    std::vector<detail::PendingSample> pendingSamples;
    std::map<ExportOutputId, core::Sha256Digest> confirmations;
    // Fingerprints of destinations this session itself published, per output.
    std::map<ExportOutputId, core::Sha256Digest> ownPublications;
    std::set<ExportOutputId> resampleAfterPublish;
    bool refreshRequested = false;
    bool gestureActive = false;
    std::uint64_t catalogueRevision = 0;
    std::string queueViewKey;
    bool pumping = false;
    // Transient jobs: runtime-only records in their own id range, never in the document.
    std::map<ExportJobId, document::ExportJob> transients;
    std::set<ExportJobId> acknowledged;
    std::uint64_t nextTransient = 1ULL << 62U;
};

namespace detail {

// `forRun` is the authoritative re-sample at Preparing: it also creates missing destination
// folders and removes this target's stale staged files left by dead processes.
[[nodiscard]] runtime::TaskHandle<std::shared_ptr<const DestinationSample>> submitDestinationSample(
    runtime::TaskScheduler& scheduler, const std::shared_ptr<ExportRunBackend>& backend,
    const SampleKey& key, std::optional<runtime::TaskGroupId> group = std::nullopt,
    std::optional<runtime::TaskOwner> owner = std::nullopt, bool forRun = false);

// Builds the published snapshot from owner-thread state. Pure apart from reading the catalogue's
// in-memory snapshot.
[[nodiscard]] std::shared_ptr<ExportQueueSnapshot> buildSnapshot(ExportQueueService::State& state);

// The context a job's outputs resolve against right now (or with the run's frozen date).
[[nodiscard]] ExportDestinationContext destinationContext(const ExportQueueService::State& state,
                                                          const document::ExportJob& job,
                                                          const JobRuntime* runtime);

// Every job the queue shows: the project's in run order, then the transient ones.
[[nodiscard]] std::vector<const document::ExportJob*>
allRecords(const ExportQueueService::State& state);
[[nodiscard]] const document::ExportJob* findRecord(const ExportQueueService::State& state,
                                                    ExportJobId id);

[[nodiscard]] std::optional<std::uint64_t>
estimateBytes(const document::ExportOutputSettings& settings,
              const document::Composition& composition, const ExportResolvedRange& range);

} // namespace detail
} // namespace bloom::host
