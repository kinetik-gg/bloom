#include <bloom/scripting/render.hpp>

#include <bloom/core/file_uri.hpp>
#include <bloom/host/export_job_runner.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>

#include <chrono>
#include <ctime>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

// ADR 0033 section 13, "One runner": a scripted render is a transient, unsaved export job run by
// host::ExportJobRunner, the runner behind the Export panel, the export queue and bloom-cli export.
// The request shape and the result fields are unchanged; the frames go through the same attempt,
// policy approval, publication and verification as every other export.
namespace bloom::scripting {
namespace {

[[nodiscard]] RenderResult failed(std::string diagnostic) {
    return RenderResult{.succeeded = false,
                        .publishedFrames = 0,
                        .preservationReport = {},
                        .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] std::optional<document::ExportFormat> formatFor(const output::OutputPresetV1 preset) {
    using P = output::OutputPresetV1;
    using F = document::ExportFormat;
    switch (preset) {
    case P::PngRgba8SrgbV1:
        return F::Png;
    case P::FlatExrRgba32fLinRec709SceneV1:
        return F::OpenExr;
    case P::TiffRgba16SrgbV1:
        return F::Tiff;
    case P::ProResMovV1:
        return F::ProResMov;
    case P::DnxhrMxfV1:
        return F::DnxhrMxf;
    case P::PcmWavV1:
        return F::Wav;
    case P::H264MovV1:
        return F::H264Mov;
    case P::H264Mp4V1:
        return F::H264Mp4;
    case P::HevcMovV1:
        return F::HevcMov;
    case P::HevcMp4V1:
        return F::HevcMp4;
    case P::Vp9WebmV1:
        return F::Vp9Webm;
    case P::Av1WebmV1:
        return F::Av1Webm;
    case P::Av1Mp4V1:
        return F::Av1Mp4;
    }
    return std::nullopt;
}

// The summary bloom-cli prints, unchanged: the preset id, how many facets were exact, and whether
// the analysis was approvable (every published frame was).
[[nodiscard]] std::string reportText(const output::OutputPresetV1 preset,
                                     const host::ExportOutputVerification& verification,
                                     const bool approvable) {
    std::size_t exact = 0;
    for (const auto& facet : verification.facets)
        if (facet.state == output::OutputPreservationStateV1::Exact)
            ++exact;
    const auto identity = output::outputPresetIdentityV1(preset);
    const auto name = identity ? std::string(identity->serializedId) : std::string("unknown");
    return "preset=" + name + "; exact-facets=" + std::to_string(exact) + "/" +
           std::to_string(verification.facets.size()) +
           "; approvable=" + (approvable ? "true" : "false");
}

[[nodiscard]] host::ExportDestinationContext
destinationContext(const std::filesystem::path& projectFolder, const std::string& compositionName,
                   const host::ExportResolvedRange& range) {
    host::ExportDestinationContext context;
    if (!projectFolder.empty())
        context.projectFolder = projectFolder;
    context.compositionName = compositionName;
    context.range = range;
    const auto now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    context.year = static_cast<std::uint16_t>(local.tm_year + 1900);
    context.month = static_cast<std::uint8_t>(local.tm_mon + 1);
    context.day = static_cast<std::uint8_t>(local.tm_mday);
    return context;
}

} // namespace

std::optional<runtime::EvaluationColorIntent>
captureRenderColorIntent(const SessionRenderSourceSnapshot& source,
                         const document::Composition& composition) {
    if (!source.colorSettings.has_value())
        return std::nullopt;
    const auto& settings = *source.colorSettings;
    std::string_view uri;
    if (const auto* builtIn =
            std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator))
        uri = builtIn->uri;
    else if (const auto* relativeArchive =
                 std::get_if<document::ProjectRelativeOciozLocator>(&settings.ocioConfig.locator))
        uri = relativeArchive->path;
    else if (const auto* externalArchive =
                 std::get_if<document::ExternalOciozLocator>(&settings.ocioConfig.locator))
        uri = externalArchive->uri;
    else if (const auto* externalConfig =
                 std::get_if<document::ExternalOcioConfigLocator>(&settings.ocioConfig.locator))
        uri = externalConfig->uri;
    const auto& working = composition.workingColorSpaceId();
    return runtime::captureEvaluationColorIntent(
        {.workingColorSpaceId = working.has_value()
                                    ? std::string_view(*working)
                                    : std::string_view(settings.processColorSpaceId),
         .ocioConfigRevision = settings.ocioConfig.expectedRevision.digest,
         .ocioConfigUri = uri});
}

RenderResult Render::run(Session& session, runtime::TaskScheduler& scheduler,
                         const runtime::SnapshotCompiler& compiler, RenderRequest request,
                         std::filesystem::path scratchDirectory,
                         std::shared_ptr<host::GpuExportProvider> injectedGpuProvider) {
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
    if (!source.colorSettings.has_value() || !captureRenderColorIntent(source, *composition))
        return failed("Project color settings are unavailable for render");
    if (!request.frame.has_value() && !request.range.has_value()) {
        return failed("Render requires a frame or range");
    }
    const auto format = formatFor(request.preset);
    auto* artifacts = session.artifactCoordinator();
    auto* publications = session.publicationCoordinator();
    if (!format || artifacts == nullptr || publications == nullptr)
        return failed("Export services are unavailable");

    // The transient job: one output, never saved, replacing the destination as Render always did.
    document::ExportJob job;
    job.id = document::ExportJobId::fromRaw(1);
    job.composition = request.composition;
    job.render.range = request.range
                           ? document::ExportRange{document::ExportRangeKind::Custom,
                                                   request.range->first, request.range->second}
                           : document::ExportRange{document::ExportRangeKind::Frame, *request.frame,
                                                   *request.frame};
    document::ExportOutput output;
    output.id = document::ExportOutputId::fromRaw(1);
    output.settings.format = *format;
    output.settings.includeAudio = !document::isImageExportFormat(*format);
    const auto destination = std::filesystem::absolute(request.destination).lexically_normal();
    const auto uri = core::fileUriFromAbsolutePath(destination);
    if (!uri)
        return failed("The render destination cannot be represented as a file URI");
    output.destination = {.portability = "external",
                          .path = *uri,
                          .overwrite = document::ExportOverwritePolicy::Replace};
    job.outputs.push_back(output);
    const auto range = host::resolveExportRange(job, snapshot.project());
    if (!range)
        return failed(request.range ? "The range is outside the composition"
                                    : "Frame is outside the composition range");
    const auto context = destinationContext(source.assetBaseDirectory, composition->name(), *range);
    auto resolved = host::resolveDestination(job, output, context);
    if (!resolved.issues.empty())
        return failed(resolved.issues.front().reason);

    // The default scratch folder sits beside the destination. It is never created when the
    // destination's folder is missing: that refusal belongs to the frame's target preflight.
    std::error_code scratchError;
    if (scratchDirectory.empty()) {
        scratchDirectory = destination.parent_path() / ".bloom-export-scratch";
        std::error_code probe;
        if (std::filesystem::is_directory(destination.parent_path(), probe))
            std::filesystem::create_directories(scratchDirectory, scratchError);
    } else {
        std::filesystem::create_directories(scratchDirectory, scratchError);
    }
    if (scratchError)
        return failed("The export scratch directory could not be created");

    // One caller-owned root shared by still/range/concurrent requests, including CPU fallback.
    injectedGpuProvider->prepare(scheduler);
    const bool media = !document::isImageExportFormat(*format);
    const auto retirement = media ? host::SequenceExportRetirementOwnerV1::create() : nullptr;
    host::ExportEncoderStatus encoders;
    encoders.probed = true;
    encoders.mediaWorker = media;
    host::ExportJobRunRequest runRequest{
        .snapshot = snapshot,
        .job = job,
        .compositionName = composition->name(),
        .range = *range,
        .colorSettings = *source.colorSettings,
        .assetBaseDirectory = source.assetBaseDirectory,
        .outputs = {host::ExportRunOutput{.record = output,
                                          .context = context,
                                          .destination = std::move(resolved),
                                          .confirmedSample = std::nullopt,
                                          .refuseExisting = false,
                                          .artifactByteLimit = std::nullopt}},
        .encoders = encoders,
        .owner = {runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)},
        .group = std::nullopt};
    RenderResult result = failed({});
    {
        host::ExportJobRunner runner(
            host::ExportJobRunnerDependencies{
                .scheduler = scheduler,
                .compiler = compiler,
                .publications = *publications,
                .artifacts = *artifacts,
                .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
                .gpu = injectedGpuProvider,
                .retirement = retirement,
                .displayProcessor = nullptr,
                .scratchDirectory = scratchDirectory},
            std::move(runRequest));
        bool stopping = false;
        // The run's technical detail (stage, typed codes) for the scripting diagnostic.
        std::string details;
        const auto collectLog = [&runner, &details] {
            for (const auto& entry : runner.takeLog())
                if (entry.severity == runtime::DiagnosticSeverity::Error)
                    details += (details.empty() ? "" : "; ") + entry.message;
        };
        while (true) {
            runner.poll();
            collectLog();
            if (runner.finished())
                break;
            if (!stopping && request.cancelled && request.cancelled()) {
                runner.stop();
                stopping = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto outputs = runner.outputs();
        const auto* state = outputs.empty() ? nullptr : &outputs.front();
        const bool published =
            state != nullptr && (state->state == host::ExportOutputState::Published ||
                                 state->state == host::ExportOutputState::PublishedWithWarnings);
        result.succeeded = published && !runner.failure() && !runner.stopped();
        if (state != nullptr && state->verification) {
            const auto& verification = *state->verification;
            const auto& provenance = verification.provenance;
            result.publishedFrames = verification.artifacts.publishedArtifacts;
            result.preservationReport = reportText(request.preset, verification, result.succeeded);
            result.gpuEvaluatedFrames = provenance.gpuFrames;
            result.gpuNativeDispatches = provenance.gpuNativeDispatches;
            result.gpuReadbacks = provenance.gpuReadbacks;
            result.gpuReadbackSubmissions = provenance.gpuReadbackSubmissions;
            result.gpuTransferredPayloads = provenance.gpuTransferredPayloads;
            result.gpuProcessPayloadBytes = provenance.gpuProcessPayloadBytes;
            result.gpuEncodedPayloadBytes = provenance.gpuEncodedPayloadBytes;
            result.gpuDeviceOwnershipEpoch = provenance.gpuDeviceOwnershipEpoch;
        }
        if (!result.succeeded) {
            if (runner.stopped())
                result.diagnostic = "The render was cancelled";
            else if (const auto& failure = runner.failure())
                result.diagnostic = failure->reason;
            else if (state != nullptr && state->failure)
                result.diagnostic = state->failure->reason;
            else if (state != nullptr && state->verification)
                result.diagnostic = "The render did not pass verification";
            else
                result.diagnostic = "The render did not publish";
            collectLog();
            if (!details.empty())
                result.diagnostic += " Details: " + details;
        }
    }
    if (retirement) {
        retirement->beginShutdown();
        while (!retirement->retirementComplete())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!retirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
}

} // namespace bloom::scripting
