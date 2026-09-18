#include <bloom/scripting/render.hpp>

#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>

#include <system_error>
#include <thread>
#include <utility>

namespace bloom::scripting {
namespace {

using Plan = std::shared_ptr<const runtime::CompiledCompositionPlan>;

template <typename Value>
[[nodiscard]] std::optional<runtime::TaskResult<Value>> await(runtime::TaskHandle<Value>& handle) {
    while (true) {
        if (auto result = handle.tryTakeResult(); result.has_value()) {
            return result;
        }
        std::this_thread::yield();
    }
}

[[nodiscard]] std::string reportText(const output::OutputAnalysisAttemptV1& attempt) {
    const auto& report = attempt.report();
    if (report == nullptr) {
        return "preservation report unavailable";
    }
    const auto view = report->view();
    std::size_t exact = 0;
    for (const auto& facet : view.facets) {
        if (facet.state == output::OutputPreservationStateV1::Exact) {
            ++exact;
        }
    }
    const auto presetIdentity = output::outputPresetIdentityV1(view.preset);
    const auto presetName = presetIdentity.has_value() ? std::string(presetIdentity->serializedId)
                                                       : std::string("unknown");
    return "preset=" + presetName + "; exact-facets=" + std::to_string(exact) + "/" +
           std::to_string(view.facets.size()) +
           "; approvable=" + (attempt.approvable() ? "true" : "false");
}

[[nodiscard]] RenderResult exportWithPlan(Session& session, runtime::TaskScheduler& scheduler,
                                          const Plan& plan, const core::RationalTime time,
                                          const output::OutputPresetV1 preset,
                                          const std::filesystem::path& destination,
                                          std::filesystem::path scratchDirectory,
                                          output::ExportResourceLedgerV1& ledger) {
    const auto failed = [](std::string diagnostic) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = {},
                            .diagnostic = std::move(diagnostic)};
    };
    auto* artifacts = session.artifactCoordinator();
    auto* publication = session.publicationCoordinator();
    if (artifacts == nullptr || publication == nullptr) {
        return failed("Export services are unavailable");
    }
    host::OutputAnalysisAttemptRequestV1 attemptRequest{
        .plan = plan,
        .evaluation = {.time = time,
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = std::size_t{512} * 1024U * 1024U},
        .targetPath = destination,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        .preset = preset,
        .displayProcessorProvider = nullptr};
    auto attemptBegin = host::beginOutputAnalysisAttemptV1(scheduler, *artifacts, ledger,
                                                           std::move(attemptRequest));
    if (!attemptBegin) {
        return failed("The output analysis task could not start");
    }
    auto runner = std::move(attemptBegin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    while (!outcome.has_value()) {
        outcome = runner.tryComplete();
        if (!outcome.has_value()) {
            std::this_thread::yield();
        }
    }
    if (!*outcome) {
        return failed("The output analysis failed");
    }
    const auto attempt = outcome->attempt();
    const auto digest =
        attempt == nullptr ? std::optional<core::Sha256Digest>{} : attempt->digest();
    if (attempt == nullptr || !attempt->approvable() || !digest.has_value()) {
        return {.succeeded = false,
                .preservationReport =
                    attempt == nullptr ? "preservation report unavailable" : reportText(*attempt),
                .diagnostic = "The preservation report is not approvable"};
    }
    auto approval = host::approveFrameExportV1(*publication, attempt, *digest);
    if (!approval) {
        return {.succeeded = false,
                .preservationReport = reportText(*attempt),
                .diagnostic = "The export could not be approved"};
    }
    auto request = std::move(approval).takeRequest();
    auto requestShared = std::shared_ptr<host::FrameExportRequestV1>(std::move(request));
    auto publicationResult =
        std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    if (scratchDirectory.empty()) {
        scratchDirectory = destination.parent_path() / ".bloom-export-scratch";
    }
    std::error_code scratchError;
    std::filesystem::create_directories(scratchDirectory, scratchError);
    if (scratchError) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = reportText(*attempt),
                            .diagnostic = "The export scratch directory could not be created"};
    }
    const auto scratch = std::move(scratchDirectory);
    runtime::TaskRequest taskRequest(
        "Publish frame export",
        {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
    auto submission =
        scheduler.submit<void>(std::move(taskRequest), [requestShared, publicationResult, artifacts,
                                                        scratch](runtime::TaskContext& context) {
            auto result = host::executeExportPublication(context, *artifacts,
                                                         std::move(*requestShared), scratch);
            *publicationResult = std::move(result);
            if (publicationResult->has_value() && static_cast<bool>(publicationResult->value())) {
                return runtime::TaskResult<void>::succeeded();
            }
            return runtime::TaskResult<void>::failed(runtime::TaskDiagnostic{
                .code = "bloom.scripting.export-publication-failed",
                .severity = runtime::DiagnosticSeverity::Error,
                .summary = "Frame publication failed",
                .detail = {},
                .suggestedAction = "Inspect the preservation and publication diagnostics."});
        });
    if (!submission.accepted()) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = reportText(*attempt),
                            .diagnostic = "The export publication task could not start"};
    }
    auto taskResult = await(submission.handle);
    const bool publicationSucceeded =
        publicationResult->has_value() ? static_cast<bool>(publicationResult->value()) : false;
    const bool succeeded = taskResult.has_value() &&
                           taskResult->state() == runtime::TaskState::Succeeded &&
                           publicationSucceeded;
    return {.succeeded = succeeded,
            .publishedFrames = succeeded ? 1U : 0U,
            .preservationReport = reportText(*attempt),
            .diagnostic = succeeded ? std::string{} : "The export publication failed"};
}

[[nodiscard]] RenderResult compilePlan(Session& session, runtime::TaskScheduler& scheduler,
                                       const runtime::SnapshotCompiler& compiler,
                                       document::CompositionId composition, Plan& plan) {
    const auto failed = [](std::string diagnostic) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = {},
                            .diagnostic = std::move(diagnostic)};
    };
    const auto snapshot = session.snapshot();
    runtime::TaskRequest request(
        "Compile composition for scripting render",
        {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
    auto submission = scheduler.submit<Plan>(
        std::move(request), [snapshot, composition, &compiler](runtime::TaskContext& context) {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<Plan>::cancelled();
            }
            const auto compiled = compiler.compile(
                {.snapshot = snapshot, .compositionId = composition}, context.cancellation());
            if (compiled.status != runtime::SnapshotCompileStatus::Compiled ||
                compiled.plan == nullptr) {
                return runtime::TaskResult<Plan>::failed(
                    runtime::TaskDiagnostic{.code = "bloom.scripting.render-compile-failed",
                                            .severity = runtime::DiagnosticSeverity::Error,
                                            .summary = "The composition could not be compiled",
                                            .detail = {},
                                            .suggestedAction = "Inspect the composition graph."});
            }
            return runtime::TaskResult<Plan>::succeeded(compiled.plan);
        });
    if (!submission.accepted()) {
        return failed("The render compile task could not start");
    }
    const auto result = await(submission.handle);
    if (!result.has_value() || result->state() != runtime::TaskState::Succeeded ||
        !result->value().has_value()) {
        return failed("The composition could not be compiled");
    }
    plan = *result->value();
    return RenderResult{
        .succeeded = true, .publishedFrames = 0, .preservationReport = {}, .diagnostic = {}};
}

} // namespace

RenderResult Render::run(Session& session, runtime::TaskScheduler& scheduler,
                         const runtime::SnapshotCompiler& compiler, RenderRequest request,
                         std::filesystem::path scratchDirectory) {
    const auto failed = [](std::string diagnostic) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = {},
                            .diagnostic = std::move(diagnostic)};
    };
    if (!session.isValid() || request.destination.empty()) {
        return failed("Render request is invalid");
    }
    const auto snapshot = session.snapshot();
    const auto* composition = snapshot.project().findComposition(request.composition);
    if (composition == nullptr) {
        return failed("Composition does not exist");
    }
    if (!request.frame.has_value() && !request.range.has_value()) {
        return failed("Render requires a frame or range");
    }
    Plan plan;
    auto compileResult = compilePlan(session, scheduler, compiler, request.composition, plan);
    if (!compileResult.succeeded) {
        return compileResult;
    }
    output::ExportResourceLedgerV1 ledger;
    if (request.range.has_value()) {
        const auto [first, last] = *request.range;
        host::FrameRangeRequestV1 range{.destination = request.destination,
                                        .firstFrame = first,
                                        .lastFrame = last,
                                        .frameRate = composition->format().frameRate(),
                                        .duration = composition->duration()};
        RenderResult aggregate{
            .succeeded = true, .publishedFrames = 0, .preservationReport = {}, .diagnostic = {}};
        const auto rangeResult =
            host::FrameRangeRunnerV1::run(range, [&](const host::FrameRangeFrameV1& frame) {
                const auto current =
                    exportWithPlan(session, scheduler, plan, frame.time, request.preset, frame.path,
                                   scratchDirectory, ledger);
                if (!current.succeeded) {
                    aggregate = current;
                    return false;
                }
                aggregate.publishedFrames += 1;
                aggregate.preservationReport = current.preservationReport;
                return true;
            });
        if (!rangeResult.succeeded()) {
            if (aggregate.diagnostic.empty()) {
                aggregate.diagnostic = rangeResult.diagnostic;
            }
            aggregate.succeeded = false;
        }
        return aggregate;
    }
    const auto time =
        host::FrameRangeRunnerV1::timeForFrame({.destination = request.destination,
                                                .firstFrame = *request.frame,
                                                .lastFrame = *request.frame,
                                                .frameRate = composition->format().frameRate(),
                                                .duration = composition->duration()},
                                               *request.frame);
    if (!time.has_value()) {
        return failed("Frame is outside the composition range");
    }
    return exportWithPlan(session, scheduler, plan, *time, request.preset, request.destination,
                          std::move(scratchDirectory), ledger);
}

} // namespace bloom::scripting
