// ADR 0033 section 13, "One runner": Render::run now runs a transient job through
// host::ExportJobRunner. This test keeps the previous Render::run implementation verbatim as the
// reference (legacyRender) and proves the new route writes the same files with the same bytes for
// the deterministic image formats, a still and a range, and the same result summary.
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/frame_range_runner.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::scripting {
namespace {

using Plan = std::shared_ptr<const runtime::CompiledCompositionPlan>;

[[nodiscard]] std::string
analysisFailureDiagnostic(const host::OutputAnalysisAttemptFailureV1* failure) {
    if (!failure)
        return "The output analysis failed";
    std::string diagnostic = "The output analysis failed during ";
    switch (failure->stage()) {
    case host::OutputAnalysisAttemptStageV1::Resolving:
        diagnostic += "target preflight";
        break;
    case host::OutputAnalysisAttemptStageV1::ColorPreparing:
        diagnostic += "color preparation";
        break;
    case host::OutputAnalysisAttemptStageV1::Evaluating:
        diagnostic += "frame evaluation";
        break;
    case host::OutputAnalysisAttemptStageV1::Identifying:
        diagnostic += "frame identity preparation";
        break;
    case host::OutputAnalysisAttemptStageV1::Analyzing:
        diagnostic += "preservation analysis";
        break;
    }
    if (failure->cancelled())
        return diagnostic + ": cancelled";
    if (const auto* code = failure->payloadAs<runtime::GpuProcessFrameDiagnosticCode>()) {
        if (*code == runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity)
            diagnostic += ": processing capacity is not ready";
        else if (*code == runtime::GpuProcessFrameDiagnosticCode::BudgetRefused)
            diagnostic += ": available processing capacity is insufficient";
        return diagnostic + " (processing code " + std::to_string(static_cast<unsigned>(*code)) +
               ")";
    }
    if (const auto* code = failure->payloadAs<runtime::EvaluationDiagnosticCode>())
        return diagnostic + ": " + std::string(runtime::evaluationDiagnosticCodeId(*code));
    const auto typedCode = [&diagnostic](const char* category, const auto code) {
        return diagnostic + " (" + category + " error " +
               std::to_string(static_cast<unsigned>(code)) + ")";
    };
    if (const auto* code = failure->payloadAs<platform::StagedArtifactError>())
        return typedCode("destination", *code);
    if (const auto* code = failure->payloadAs<output::ProcessFrameSemanticIdentityErrorCode>())
        return typedCode("frame identity", *code);
    if (const auto* code = failure->payloadAs<output::OutputAnalysisAnalyzerErrorCodeV1>())
        return typedCode("preservation analysis", *code);
    if (const auto* code = failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>())
        return typedCode("attempt", *code);
    if (const auto* code = failure->payloadAs<runtime::TaskSubmissionStatus>())
        return typedCode("task submission", *code);
    return diagnostic;
}

template <typename Value>
[[nodiscard]] std::optional<runtime::TaskResult<Value>>
await(runtime::TaskHandle<Value>& handle, const std::function<bool()>& cancelled) {
    while (true) {
        if (cancelled && cancelled())
            handle.cancel();
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

[[nodiscard]] RenderResult
exportWithPlan(Session& session, runtime::TaskScheduler& scheduler, const Plan& plan,
               const std::filesystem::path& assetBaseDirectory, const core::RationalTime time,
               const runtime::EvaluationColorIntent& colorIntent,
               const output::OutputPresetV1 preset, const std::filesystem::path& destination,
               std::filesystem::path scratchDirectory, output::ExportResourceLedgerV1& ledger,
               const std::shared_ptr<host::GpuExportProvider>& gpuProvider,
               const std::function<bool()>& cancelled,
               const std::shared_ptr<const runtime::CpuCompositionEvaluator>& fallbackEvaluator) {
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
                       .colorIntent = colorIntent,
                       .pixelStorageByteLimit = 0},
        .assetBaseDirectory = assetBaseDirectory,
        .fallbackEvaluator = fallbackEvaluator,
        .targetPath = destination,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        .preset = preset,
        .displayProcessorProvider = nullptr,
        .gpuProvider = gpuProvider,
        .useRootPixelCapacity = true};
    auto attemptBegin = host::beginOutputAnalysisAttemptV1(scheduler, *artifacts, ledger,
                                                           std::move(attemptRequest));
    if (!attemptBegin) {
        return failed("The output analysis task could not start");
    }
    auto runner = std::move(attemptBegin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    while (!outcome.has_value()) {
        if (cancelled && cancelled())
            runner.requestCancellation();
        outcome = runner.tryComplete();
        if (!outcome.has_value()) {
            std::this_thread::yield();
        }
    }
    // Carry the geniune native provenance this attempt observed (empty for a CPU fallback) on every
    // result that consumes the completed attempt, including its failure and non-approvable paths.
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> gpuProvenance;
    if (*outcome) {
        gpuProvenance = outcome->gpuProvenance();
    }
    const auto finish = [&gpuProvenance](RenderResult result) {
        if (gpuProvenance.has_value()) {
            result.gpuEvaluatedFrames = gpuProvenance->gpuEvaluated() ? 1U : 0U;
            result.gpuNativeDispatches = gpuProvenance->counters.nativeDispatches;
            result.gpuReadbacks = gpuProvenance->counters.readbacks;
            result.gpuReadbackSubmissions = gpuProvenance->readbackSubmissions;
            result.gpuTransferredPayloads = gpuProvenance->transferredPayloads;
            result.gpuProcessPayloadBytes = gpuProvenance->processPayloadBytes;
            result.gpuEncodedPayloadBytes = gpuProvenance->encodedPayloadBytes;
            result.gpuDeviceOwnershipEpoch = gpuProvenance->deviceOwnershipEpoch;
        }
        return result;
    };
    if (!*outcome) {
        return finish(failed(analysisFailureDiagnostic(outcome->failure())));
    }
    const auto attempt = outcome->attempt();
    const auto digest =
        attempt == nullptr ? std::optional<core::Sha256Digest>{} : attempt->digest();
    if (attempt == nullptr || !attempt->approvable() || !digest.has_value()) {
        return finish({.succeeded = false,
                       .preservationReport = attempt == nullptr ? "preservation report unavailable"
                                                                : reportText(*attempt),
                       .diagnostic = "The preservation report is not approvable"});
    }
    auto approval = host::approveFrameExportV1(*publication, attempt, *digest);
    if (!approval) {
        return finish({.succeeded = false,
                       .preservationReport = reportText(*attempt),
                       .diagnostic = "The export could not be approved"});
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
        return finish({.succeeded = false,
                       .preservationReport = reportText(*attempt),
                       .diagnostic = "The export scratch directory could not be created"});
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
        return finish({.succeeded = false,
                       .preservationReport = reportText(*attempt),
                       .diagnostic = "The export publication task could not start"});
    }
    auto taskResult = await(submission.handle, cancelled);
    const bool publicationSucceeded =
        publicationResult->has_value() ? static_cast<bool>(publicationResult->value()) : false;
    const bool succeeded = taskResult.has_value() &&
                           taskResult->state() == runtime::TaskState::Succeeded &&
                           publicationSucceeded;
    return finish({.succeeded = succeeded,
                   .publishedFrames = succeeded ? 1U : 0U,
                   .preservationReport = reportText(*attempt),
                   .diagnostic = succeeded ? std::string{} : "The export publication failed"});
}

[[nodiscard]] RenderResult compilePlan(const document::Snapshot& snapshot,
                                       runtime::TaskScheduler& scheduler,
                                       const runtime::SnapshotCompiler& compiler,
                                       document::CompositionId composition, Plan& plan,
                                       const std::function<bool()>& cancelled) {
    const auto failed = [](std::string diagnostic) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = {},
                            .diagnostic = std::move(diagnostic)};
    };
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
    const auto result = await(submission.handle, cancelled);
    if (!result.has_value() || result->state() != runtime::TaskState::Succeeded ||
        !result->value().has_value()) {
        return failed("The composition could not be compiled");
    }
    plan = *result->value();
    return RenderResult{
        .succeeded = true, .publishedFrames = 0, .preservationReport = {}, .diagnostic = {}};
}

RenderResult legacyRender(Session& session, runtime::TaskScheduler& scheduler,
                          const runtime::SnapshotCompiler& compiler, RenderRequest request,
                          std::filesystem::path scratchDirectory,
                          std::shared_ptr<host::GpuExportProvider> injectedGpuProvider) {
    const auto failed = [](std::string diagnostic) {
        return RenderResult{.succeeded = false,
                            .publishedFrames = 0,
                            .preservationReport = {},
                            .diagnostic = std::move(diagnostic)};
    };
    if (!session.isValid() || request.destination.empty() ||
        (request.cancelled && request.cancelled())) {
        return failed("Render request is invalid");
    }
    if (!injectedGpuProvider) {
        auto missing = failed(
            "Render requires an explicitly owned processing provider. Share one GpuExportProvider "
            "across requests; a disabled provider supports CPU rendering.");
        missing.diagnosticCode = RenderDiagnosticCode::MissingProcessingRoot;
        return missing;
    }
    const auto source = session.renderSourceSnapshot();
    const auto& snapshot = source.snapshot;
    const auto* composition = snapshot.project().findComposition(request.composition);
    if (composition == nullptr) {
        return failed("Composition does not exist");
    }
    const auto colorIntent = captureRenderColorIntent(source, *composition);
    if (!colorIntent.has_value())
        return failed("Project color settings are unavailable for render");
    if (!request.frame.has_value() && !request.range.has_value()) {
        return failed("Render requires a frame or range");
    }
    Plan plan;
    auto compileResult =
        compilePlan(snapshot, scheduler, compiler, request.composition, plan, request.cancelled);
    if (!compileResult.succeeded) {
        return compileResult;
    }
    output::ExportResourceLedgerV1 ledger;
    // One caller-owned root shared by still/range/concurrent requests, including CPU fallback.
    auto gpuProvider = std::move(injectedGpuProvider);
    gpuProvider->prepare(scheduler);
    // One final-render evaluator per request: its verified source revisions and video sessions
    // make each frame's CPU fallback revalidation an identity check instead of a source read.
    const auto fallbackEvaluator = std::make_shared<const runtime::CpuCompositionEvaluator>();
    if (request.range.has_value()) {
        const auto [first, last] = *request.range;
        host::FrameRangeRequestV1 range{.destination = request.destination,
                                        .firstFrame = first,
                                        .lastFrame = last,
                                        .frameRate = composition->format().frameRate(),
                                        .duration = composition->duration()};
        RenderResult aggregate{
            .succeeded = true, .publishedFrames = 0, .preservationReport = {}, .diagnostic = {}};
        const auto rangeResult = host::FrameRangeRunnerV1::run(
            range,
            [&](const host::FrameRangeFrameV1& frame) {
                const auto current =
                    exportWithPlan(session, scheduler, plan, source.assetBaseDirectory, frame.time,
                                   *colorIntent, request.preset, frame.path, scratchDirectory,
                                   ledger, gpuProvider, request.cancelled, fallbackEvaluator);
                aggregate.gpuEvaluatedFrames += current.gpuEvaluatedFrames;
                aggregate.gpuNativeDispatches += current.gpuNativeDispatches;
                aggregate.gpuReadbacks += current.gpuReadbacks;
                aggregate.gpuReadbackSubmissions += current.gpuReadbackSubmissions;
                aggregate.gpuTransferredPayloads += current.gpuTransferredPayloads;
                aggregate.gpuProcessPayloadBytes += current.gpuProcessPayloadBytes;
                aggregate.gpuEncodedPayloadBytes += current.gpuEncodedPayloadBytes;
                if (current.gpuDeviceOwnershipEpoch != 0) {
                    aggregate.gpuDeviceOwnershipEpoch = current.gpuDeviceOwnershipEpoch;
                }
                if (!current.succeeded) {
                    aggregate.succeeded = false;
                    aggregate.preservationReport = current.preservationReport;
                    aggregate.diagnostic = current.diagnostic;
                    return false;
                }
                aggregate.publishedFrames += 1;
                aggregate.preservationReport = current.preservationReport;
                return true;
            },
            request.cancelled);
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
    return exportWithPlan(session, scheduler, plan, source.assetBaseDirectory, *time, *colorIntent,
                          request.preset, request.destination, std::move(scratchDirectory), ledger,
                          gpuProvider, request.cancelled, fallbackEvaluator);
}

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::string> names(const std::filesystem::path& folder) {
    std::vector<std::string> list;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.is_regular_file())
            list.push_back(entry.path().filename().string());
    std::ranges::sort(list);
    return list;
}

void compare(const std::filesystem::path& legacy, const std::filesystem::path& current,
             const std::string& label) {
    const auto legacyNames = names(legacy);
    const auto currentNames = names(current);
    expect(!legacyNames.empty() && legacyNames == currentNames,
           label + ": the same files are written");
    for (const auto& name : legacyNames)
        expect(bytes(legacy / name) == bytes(current / name),
               label + ": identical bytes for " + name);
}

void testParity() {
    auto created = Session::createNew("Parity", "Composition", core::RationalTime::fromInteger(2));
    expect(static_cast<bool>(created), "session");
    if (!created)
        return;
    auto session = std::move(created).takeSession();
    const auto script = parseScriptJson(
        R"([{"op":"bloom.layer.add-solid","args":{"composition":1,"name":"Solid","color":[0.8,0.2,0.1,1],"position":[960,540]}},{"op":"bloom.layer.add-text","args":{"composition":1,"name":"Text","text":"PARITY","position":[900,500],"size":64,"color":[1,1,1,1]}}])");
    expect(static_cast<bool>(script), "script parses");
    if (!script)
        return;
    for (const auto& transaction : script.transactions())
        expect(
            session->executeJsonOperation(transaction.operation, transaction.arguments).succeeded(),
            "script applies");
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    runtime::TaskScheduler scheduler(config);
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    const auto provider = host::GpuExportProvider::create(options);
    const auto root = std::filesystem::temp_directory_path() / "bloom-render-parity";
    std::filesystem::remove_all(root);
    const auto composition = session->snapshot().project().compositions().front().id();
    struct Case final {
        std::string label;
        output::OutputPresetV1 preset;
        std::string file;
        std::optional<std::uint64_t> frame;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
    };
    const std::vector<Case> cases{
        {"png still", output::OutputPresetV1::PngRgba8SrgbV1, "still.png", 12, std::nullopt},
        {"png range", output::OutputPresetV1::PngRgba8SrgbV1, "range.png", std::nullopt,
         std::pair<std::uint64_t, std::uint64_t>{3, 6}},
        {"exr still", output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1, "still.exr", 7,
         std::nullopt},
        {"exr range", output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1, "range.exr",
         std::nullopt, std::pair<std::uint64_t, std::uint64_t>{0, 2}},
        {"tiff still", output::OutputPresetV1::TiffRgba16SrgbV1, "still.tiff", 5, std::nullopt},
    };
    for (const auto& item : cases) {
        const auto legacyFolder = root / ("legacy-" + item.label);
        const auto currentFolder = root / ("current-" + item.label);
        std::filesystem::create_directories(legacyFolder);
        std::filesystem::create_directories(currentFolder);
        const auto request = [&](const std::filesystem::path& folder) {
            return RenderRequest{.composition = composition,
                                 .frame = item.frame,
                                 .range = item.range,
                                 .preset = item.preset,
                                 .destination = folder / item.file};
        };
        const auto legacy = legacyRender(*session, scheduler, compiler, request(legacyFolder),
                                         root / "scratch-legacy", provider);
        const auto current = Render::run(*session, scheduler, compiler, request(currentFolder),
                                         root / "scratch-current", provider);
        if (!legacy.succeeded) {
            // TIFF needs the media worker; both routes must agree when it is absent.
            expect(!current.succeeded, item.label + ": both routes refuse together");
            std::cout << "NOTE: " << item.label << " unavailable here: " << legacy.diagnostic
                      << '\n';
            continue;
        }
        expect(current.succeeded,
               item.label + ": the runner route succeeds: " + current.diagnostic);
        expect(current.publishedFrames == legacy.publishedFrames &&
                   current.preservationReport == legacy.preservationReport,
               item.label + ": the same frame count and report (" + current.preservationReport +
                   " vs " + legacy.preservationReport + ")");
        expect(current.gpuEvaluatedFrames == legacy.gpuEvaluatedFrames,
               item.label + ": the same device provenance");
        compare(legacyFolder, currentFolder, item.label);
    }
    // A cancelled request never starts work; a missing provider is a typed refusal.
    const auto cancelled =
        Render::run(*session, scheduler, compiler,
                    RenderRequest{.composition = composition,
                                  .frame = 1,
                                  .range = std::nullopt,
                                  .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                  .destination = root / "cancelled.png",
                                  .cancelled = [] { return true; }},
                    {}, provider);
    expect(!cancelled.succeeded && !std::filesystem::exists(root / "cancelled.png"),
           "a cancelled render publishes nothing");
    const auto outside = Render::run(*session, scheduler, compiler,
                                     RenderRequest{.composition = composition,
                                                   .frame = 100000,
                                                   .range = std::nullopt,
                                                   .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                                   .destination = root / "outside.png"},
                                     {}, provider);
    expect(!outside.succeeded && outside.diagnostic == "Frame is outside the composition range",
           "a frame outside the composition is refused as before");
    expect(provider->shutdownAndWait(std::chrono::seconds(10)), "the provider retires");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace bloom::scripting

int main() {
    bloom::scripting::testParity();
    return bloom::scripting::failures == 0 ? 0 : 1;
}
