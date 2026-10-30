// ADR 0033: ExportQueueService behind the runner seam, driven by the deterministic fake runner
// (src/host/testing) the src/ui panel tests also use. No evaluation, encoding or filesystem
// sampling happens here; every transition is one pump of the fake.
#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace {

using namespace bloom;
using host::ExportCommand;
using host::ExportJobState;
using host::ExportUnavailableCode;
using host::testing::FakeExportScript;

document::Snapshot currentSnapshot(const host::ProjectSession& session) {
    const auto decoded = session.decodedSnapshot();
    return decoded.snapshot();
}

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

host::ExportEncoderStatus encoders(const bool platform) {
    host::ExportEncoderStatus status;
    status.exportPlatform = platform;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = host::ExportEncoderStatus::OpenH264::Installed;
    return status;
}

struct Fixture {
    host::ProjectSessionIdentitySource identity;
    host::ProjectSession session = [this] {
        auto created = document::makeNewProject("Queue", "Main", core::RationalTime::fromInteger(1),
                                                *document::CompositionFormat::create(64, 32));
        auto result = host::ProjectSession::createDecoded(
            identity, {.project = std::move(created.project),
                       .colorSettings = document::makeBloomNeutralColorSettingsV1({}),
                       .editability = host::DecodedProjectEditability::Editable,
                       .displayPath = host::ProjectDisplayPath::create("/projects/queue.bloom"),
                       .persistedAllocatorHighWater = std::nullopt});
        if (!result)
            throw std::runtime_error("session");
        return std::move(result).takeSession();
    }();
    runtime::NodeDefinitionRegistry registry;
    runtime::SnapshotCompiler compiler{registry};
    runtime::TaskScheduler scheduler;
    host::PublicationCoordinator publications = *host::PublicationCoordinator::create();
    platform::StagedArtifactCoordinator artifacts =
        platform::StagedArtifactCoordinator::create({}).takeCoordinator();
    std::shared_ptr<host::testing::FakeExportRunBackend> fake =
        std::make_shared<host::testing::FakeExportRunBackend>();
    std::unique_ptr<host::ExportQueueService> service;

    explicit Fixture(const bool platform = true) {
        service = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
            .session = session,
            .scheduler = scheduler,
            .compiler = compiler,
            .publications = publications,
            .artifacts = artifacts,
            .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
            .gpu = nullptr,
            .retirement = nullptr,
            .presets = host::ExportPresetCatalogue::create(
                {.userDataDirectory = std::nullopt, .initialEncoders = encoders(platform)}),
            .limits = {},
            .displayProcessor = nullptr,
            .scratchDirectory = {},
            .backend = fake});
    }
    ~Fixture() {
        service.reset();
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    document::ExportJobId addJob(document::ExportOverwritePolicy policy =
                                     document::ExportOverwritePolicy::AskBeforeReplacing) {
        document::ExportJob job;
        job.composition = currentSnapshot(session).project().compositions().front().id();
        job.render.range = {document::ExportRangeKind::Custom, 0, 3};
        document::ExportOutput output;
        output.presetId = "builtin:h264-mp4";
        output.destination = {"project-relative", "Exports/{name}.mp4", policy};
        job.outputs = {output};
        commands::Transaction transaction("Add");
        transaction.emplace<commands::AddExportJob>(job);
        const auto result = session.execute(std::move(transaction));
        return *result.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
    }

    std::optional<host::ExportJobView> view(const document::ExportJobId id) const {
        for (const auto& job : service->snapshot()->jobs)
            if (job.id == id)
                return job;
        return std::nullopt;
    }

    // Pumps until the samples land (worker tasks), then one transition per pump.
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            service->pump();
            bool pending = false;
            for (const auto& job : service->snapshot()->jobs)
                for (const auto& output : job.outputs)
                    for (const auto& issue : output.preflight)
                        pending = pending ||
                                  issue.code == host::ExportPreflightCode::DestinationCheckPending;
            if (!pending)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    template <typename Predicate> bool pumpUntil(Predicate&& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            service->pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

void testSucceedsWithProgressAndWaitingForViewer() {
    Fixture f;
    f.fake->push({.framesPerPoll = 1,
                  .waitingForViewerAfterFrames = 2,
                  .waitingForViewerPolls = 3,
                  .outcome = FakeExportScript::Outcome::Succeed,
                  .failAtFrame = 0});
    const auto job = f.addJob();
    f.settle();
    expect(f.service->canStart(job).available, "the sampled job can start");
    const auto revision = f.service->snapshot()->revision;
    auto subscription = f.service->subscribe();
    expect(subscription.status == host::ExportQueueSubscribeStatus::Accepted, "subscribe");
    const document::ExportJobId ids[] = {job};
    const auto started = f.service->start(ids);
    expect(started.accepted && started.revision > revision, "start publishes a new revision");
    expect(subscription.subscription.serial() == f.service->snapshot()->revision,
           "the subscription serial follows the snapshot revision");
    bool sawRendering = false;
    bool sawProgress = false;
    bool sawWaiting = false;
    bool etaAbsentWhileWaiting = true;
    expect(f.pumpUntil([&] {
        const auto view = f.view(job);
        sawRendering = sawRendering || view->state == ExportJobState::Rendering;
        sawProgress = sawProgress || (view->progress.framesCompleted > 0 &&
                                      view->progress.framesTotal == 4 && view->progress.fraction);
        if (view->progress.route == host::ExportDeviceRoute::WaitingForViewer) {
            sawWaiting = true;
            etaAbsentWhileWaiting = etaAbsentWhileWaiting && !view->progress.remaining;
        }
        return host::isTerminal(view->state);
    }),
           "the fake job finishes");
    expect(sawRendering && sawProgress, "the job renders with progress");
    expect(sawWaiting && etaAbsentWhileWaiting, "WaitingForViewer shows no ETA");
    const auto view = f.view(job);
    expect(view->state == ExportJobState::Done && view->outputs.front().verification &&
               view->outputs.front().verification->overall == host::ExportCheckOutcome::Passed,
           "the job is Done with a passed verification");
    const auto events = f.service->events(0);
    expect(!events.events.empty() &&
               std::holds_alternative<host::ExportJobSucceeded>(events.events.front().payload) &&
               std::holds_alternative<host::ExportQueueDrained>(events.events.back().payload),
           "terminal event, then the drain");
    const auto log = f.service->log(job, 0);
    expect(!log.entries.empty() && log.entries.back().sequence == log.lastSequence,
           "the log is readable");
    expect(f.service->log(job, log.lastSequence).entries.empty(),
           "log reads resume after a sequence");
    expect(f.service->jobForTaskOwner(view->run->owner) == job, "the Jobs row maps to the job");
}

void testFailsWithVerification() {
    Fixture f;
    f.fake->push({.framesPerPoll = 2,
                  .waitingForViewerAfterFrames = 0,
                  .waitingForViewerPolls = 0,
                  .outcome = FakeExportScript::Outcome::FailVerification,
                  .failAtFrame = 0});
    const auto job = f.addJob();
    f.settle();
    const document::ExportJobId ids[] = {job};
    expect(f.service->start(ids).accepted, "start");
    expect(f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }), "finishes");
    const auto view = f.view(job);
    expect(view->state == ExportJobState::Failed && view->failure &&
               view->failure->code == host::ExportFailureCode::VerificationFailed,
           "a failed verification fails the job");
    const auto& checks = view->outputs.front().verification->checks;
    expect(std::ranges::any_of(checks,
                               [](const auto& check) {
                                   return check.check == host::ExportCheck::ColourTags &&
                                          check.outcome == host::ExportCheckOutcome::Failed;
                               }),
           "the failed check is carried");
    bool failedEvent = false;
    for (const auto& event : f.service->events(0).events)
        if (const auto* failed = std::get_if<host::ExportJobFailed>(&event.payload))
            failedEvent = failed->job == job && !failed->outputs.empty();
    expect(failedEvent, "ExportJobFailed carries the verification");
    expect(f.service->canStart(job).available &&
               f.service->availability(job, ExportCommand::Retry).available,
           "a failed job can retry");
}

void testCancelAndPause() {
    Fixture f;
    f.fake->push({});
    const auto job = f.addJob();
    f.settle();
    const document::ExportJobId ids[] = {job};
    expect(f.service->start(ids).accepted, "start");
    expect(f.pumpUntil([&] { return f.view(job)->state == ExportJobState::Rendering; }),
           "rendering");
    expect(f.service->pause(job).accepted, "pause accepted while rendering");
    expect(f.pumpUntil([&] { return f.view(job)->state == ExportJobState::Paused; }),
           "the job pauses at a frame boundary");
    expect(f.service->resume(job).accepted, "resume");
    expect(f.service->stop(job).accepted, "stop");
    expect(f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }), "stopped");
    expect(f.view(job)->state == ExportJobState::Stopped &&
               f.view(job)->stopCause == host::ExportStopCause::User,
           "the job is Stopped by the user");
    bool cancelled = false;
    for (const auto& event : f.service->events(0).events)
        cancelled = cancelled || std::holds_alternative<host::ExportJobCancelled>(event.payload);
    expect(cancelled, "ExportJobCancelled is raised");

    // Waiting -> Ready on stop.
    const auto other = f.addJob();
    f.settle();
    f.fake->push({.framesPerPoll = 1});
    const document::ExportJobId pair[] = {job, other};
    expect(f.service->start(pair).accepted, "start two");
    expect(f.view(other)->state == ExportJobState::Waiting, "the second waits");
    expect(f.service->stop(other).accepted && f.view(other)->state == ExportJobState::Ready,
           "stopping a waiting job returns it to Ready");
}

void testNeedsConfirmation() {
    Fixture f;
    const auto job = f.addJob();
    f.fake->setExisting("/projects/Exports/Main.mp4");
    f.settle();
    auto view = f.view(job);
    const auto& issues = view->outputs.front().preflight;
    const bool raised = std::ranges::any_of(issues, [](const auto& issue) {
        return issue.code == host::ExportPreflightCode::DestinationExists &&
               issue.severity == host::ExportPreflightSeverity::NeedsConfirmation &&
               issue.target == host::ExportPreflightTarget::Destination;
    });
    expect(raised, "an existing destination raises DestinationExists NeedsConfirmation");
    expect(f.service->canStart(job).code == ExportUnavailableCode::NeedsReplaceConfirmation,
           "Start waits for the confirmation");
    expect(f.service->confirmReplace(job, view->outputs.front().id).accepted, "confirm");
    expect(f.view(job)->outputs.front().replaceConfirmed && f.service->canStart(job).available,
           "a confirmed destination can start");
    // An external change clears the confirmation.
    f.fake->setExisting("/projects/Exports/Main.mp4", 2);
    f.service->refreshPreflight();
    f.settle();
    expect(!f.view(job)->outputs.front().replaceConfirmed &&
               f.service->canStart(job).code == ExportUnavailableCode::NeedsReplaceConfirmation,
           "a changed destination needs a fresh confirmation");
    // Replace warns; Refuse blocks.
    const auto replace = f.addJob(document::ExportOverwritePolicy::Replace);
    const auto refuse = f.addJob(document::ExportOverwritePolicy::Refuse);
    f.settle();
    expect(f.service->canStart(replace).available, "Replace only warns");
    expect(f.service->canStart(refuse).code == ExportUnavailableCode::PreflightBlocked,
           "Refuse blocks");
}

void testTransientAndPlatform() {
    {
        Fixture f;
        host::ExportTransientJobRequest request;
        request.composition = currentSnapshot(f.session).project().compositions().front().id();
        request.range = {document::ExportRangeKind::Frame, 0, 0};
        request.settings.format = document::ExportFormat::Png;
        request.destination = {"external", "file:///renders/capture.png",
                               document::ExportOverwritePolicy::Replace};
        const auto submitted = f.service->submitTransient(request);
        expect(submitted.accepted && submitted.job && f.view(*submitted.job)->transient,
               "a transient job is accepted and marked");
        expect(currentSnapshot(f.session).project().exportJobs().empty(), "it is not saved");
        expect(!f.service->acknowledgeTransient(*submitted.job),
               "an active transient job cannot be acknowledged");
        expect(f.pumpUntil([&] { return host::isTerminal(f.view(*submitted.job)->state); }),
               "the transient job finishes");
        expect(f.service->acknowledgeTransient(*submitted.job) && !f.view(*submitted.job),
               "an acknowledged transient job leaves the snapshot");
    }
    Fixture f(false);
    const auto job = f.addJob();
    f.settle();
    expect(f.service->canStart(job).code == ExportUnavailableCode::PlatformNotSupported &&
               f.service->availability(job, ExportCommand::EditJob).available,
           "off Linux Start is PlatformNotSupported and editing stays available");
    expect(f.fake->runsStarted() == 0, "nothing runs off Linux");
}

} // namespace

int main() {
    try {
        testSucceedsWithProgressAndWaitingForViewer();
        testFailsWithVerification();
        testCancelAndPause();
        testNeedsConfirmation();
        testTransientAndPlatform();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
