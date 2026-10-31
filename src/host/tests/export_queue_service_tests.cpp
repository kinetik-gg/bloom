// ADR 0033: the export queue's runtime owner, presets, destinations and runner, driven headlessly
// through a real ProjectSession, scheduler, staged publication and the media worker.
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/export_job_runner.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <thread>

namespace {

using namespace bloom;
using host::ExportCommand;
using host::ExportJobState;
using host::ExportUnavailableCode;

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

std::filesystem::path scratch(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() /
                ("bloom-export-queue-" + name + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::string fileUri(const std::filesystem::path& path) { return "file://" + path.string(); }

host::ExportEncoderStatus linuxEncoders(const bool openh264 = true, const bool vaapi = false) {
    host::ExportEncoderStatus status;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = openh264 ? host::ExportEncoderStatus::OpenH264::Installed
                               : host::ExportEncoderStatus::OpenH264::NotInstalled;
    status.vaapiH264 = vaapi;
    status.vaapiHevc = vaapi;
    return status;
}

// ---------------------------------------------------------------------------------------------

void testResolveDestination() {
    document::ExportJob job;
    job.name = "Final";
    job.render.range = {document::ExportRangeKind::Custom, 8, 12};
    document::ExportOutput movie;
    movie.destination.path = "Exports/{name}_{job}_{date}.{format}";
    host::ExportDestinationContext context;
    context.projectFolder = std::filesystem::path("/projects/show");
    context.compositionName = "Shot: 10";
    context.range = host::ExportResolvedRange{8, 12};
    context.year = 2026;
    context.month = 10;
    context.day = 30;
    auto resolved = host::resolveDestination(job, movie, context);
    expect(resolved.issues.empty() &&
               resolved.firstPath == "/projects/show/Exports/Shot_ 10_Final_2026-10-30.mp4" &&
               resolved.fileCount == 1,
           "tokens expand, values are sanitized and the movie is one file");

    movie.destination.path = "Exports/{nope}.mp4";
    expect(!host::resolveDestination(job, movie, context).issues.empty(),
           "an unknown token is NamingInvalid");
    movie.destination.path = "Exports/out.mov";
    expect(!host::resolveDestination(job, movie, context).issues.empty(),
           "a changed extension is NamingInvalid");
    context.compositionName = "A/B";
    movie.destination.path = "{name}.mp4";
    expect(!host::resolveDestination(job, movie, context).issues.empty(),
           "a separator from a value is NamingInvalid");
    context.compositionName = "Shot";
    movie.destination.path = "../outside.mp4";
    expect(!host::resolveDestination(job, movie, context).issues.empty(),
           "a project-relative destination stays below the project");
    movie.destination = {"external", "file:///renders/a%20b.mp4",
                         document::ExportOverwritePolicy::Replace};
    expect(host::resolveDestination(job, movie, context).firstPath == "/renders/a b.mp4",
           "an external file URI resolves");
    context.projectFolder.reset();
    movie.destination = {};
    movie.destination.path = "Exports/{name}.mp4";
    expect(!host::resolveDestination(job, movie, context).issues.empty(),
           "an unsaved project has no project-relative destination");

    document::ExportOutput frames;
    frames.settings.format = document::ExportFormat::OpenExr;
    frames.settings.naming = {1001, 4, "{job}.<base>.####.<ext>"};
    frames.destination = {"external", "file:///renders/plate.exr",
                          document::ExportOverwritePolicy::AskBeforeReplacing};
    resolved = host::resolveDestination(job, frames, context);
    expect(resolved.issues.empty() && resolved.firstPath == "/renders/Final.plate.1001.exr" &&
               resolved.lastPath == "/renders/Final.plate.1005.exr" && resolved.fileCount == 5 &&
               resolved.basePath == "/renders/plate.exr",
           "a sequence names its first and last files from the start frame");
    job.render.range = {document::ExportRangeKind::Frame, 9, 0};
    context.range = host::ExportResolvedRange{9, 9};
    resolved = host::resolveDestination(job, frames, context);
    expect(resolved.firstPath == "/renders/plate.exr" && resolved.fileCount == 1,
           "an image output with a one-frame range writes one still");
}

void testScript() {
    document::ExportJob job;
    job.composition = document::CompositionId::fromRaw(3);
    job.name = "Review";
    job.render.range = {document::ExportRangeKind::Custom, 2, 9};
    document::ExportOutput output;
    output.presetId = "builtin:h264-mp4";
    output.destination.path = "Exports/{name}.mp4";
    job.outputs = {output};
    const auto cli = host::exportJobScript(job, host::ExportScriptFormat::CliJson);
    expect(cli.find("\"op\":\"bloom.export.add-job\"") != std::string::npos &&
               cli.find("\"op\":\"export.start\"") != std::string::npos &&
               cli.find("\"first\":2") != std::string::npos,
           "the CLI script adds the job and starts it");
    const auto python = host::exportJobScript(job, host::ExportScriptFormat::Python);
    expect(python.find("bloom.ops.export.add_job(") != std::string::npos &&
               python.find("bloom.export.start()") != std::string::npos,
           "the Python script adds the job and starts it");
}

// ADR 0033 section 5: the default destination and job the panel adds, from the catalogue.
void testDefaults() {
    auto catalogue = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = linuxEncoders()});
    auto chosen = catalogue->defaultPreset();
    expect(chosen.settings.format == document::ExportFormat::H264Mp4 &&
               chosen.destination.portability == "project-relative" &&
               chosen.destination.path == "Exports/{name}.mp4" &&
               chosen.destination.overwrite == document::ExportOverwritePolicy::AskBeforeReplacing,
           "the default output is H.264 MP4 at Exports/{name}.mp4, ask before replacing");
    // H.264 ships with Bloom, so a probe without VA-API keeps the H.264 default; the fallback's
    // destination still follows its own container.
    catalogue->setEncoderStatus(linuxEncoders(false, false));
    chosen = catalogue->defaultPreset();
    expect(chosen.settings.format == document::ExportFormat::H264Mp4 &&
               host::defaultExportDestination(document::ExportFormat::ProResMov).path ==
                   "Exports/{name}.mov",
           "the fallback's destination follows its container");
    expect(host::defaultExportDestination(document::ExportFormat::OpenExr).path ==
               "Exports/{name}.exr",
           "every format has its extension in the default destination");
    const auto composition = document::CompositionId::fromRaw(7);
    const auto job = host::defaultExportJob(composition, chosen);
    expect(job.composition == composition &&
               job.render.range.kind == document::ExportRangeKind::WorkArea && job.enabled &&
               !job.labelColor && job.outputs.size() == 1 &&
               job.outputs[0].presetId == chosen.presetId &&
               job.outputs[0].settings == chosen.settings &&
               job.outputs[0].destination == chosen.destination && job.outputs[0].enabled &&
               !job.outputs[0].importIntoAssets && job.outputs[0].settings.useCachedFrames &&
               !job.id.isValid() && !job.outputs[0].id.isValid(),
           "the default job: work area, one enabled output, import off, reuse on, ids unset");
    expect(document::validateExportJob([&] {
               auto copy = job;
               copy.id = document::ExportJobId::fromRaw(1);
               copy.outputs[0].id = document::ExportOutputId::fromRaw(1);
               return copy;
           }())
               .ok(),
           "the default job is a valid record");
}

// The panel offers only these tokens per format.
void testFormatTokens() {
    const auto catalogue = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = linuxEncoders()});
    const auto snapshot = catalogue->snapshot();
    expect(snapshot->formats == host::exportFormatDescriptions() && snapshot->formats.size() == 11,
           "the catalogue lists every format");
    for (const auto& format : snapshot->formats) {
        const auto profiles = document::exportCodecProfiles(format.format);
        bool same = profiles.size() == format.codecProfiles.size();
        for (std::size_t index = 0; same && index < profiles.size(); ++index)
            same = profiles[index] == format.codecProfiles[index].token;
        expect(same,
               "codec profiles match the document vocabulary for " + std::string(format.token));
        expect(format.codecProfiles.empty() ||
                   std::ranges::any_of(
                       format.codecProfiles,
                       [&](const auto& item) { return item.token == format.defaultCodecProfile; }),
               "the default profile is one of the listed tokens");
        expect(format.extension == host::exportFormatExtension(format.format) &&
                   format.imageSequence == document::isImageExportFormat(format.format),
               "extension and sequence flag match");
        // Every listed token is accepted by the record validator.
        for (const auto& profile : format.codecProfiles) {
            document::ExportOutput output;
            output.id = document::ExportOutputId::fromRaw(1);
            output.settings.format = format.format;
            output.settings.codecProfile = std::string(profile.token);
            expect(document::validateExportOutput(output).ok(),
                   "a listed profile validates: " + std::string(profile.token));
        }
    }
    const auto& exr = host::exportFormatDescription(document::ExportFormat::OpenExr);
    expect(exr.compressions.size() == 4 && exr.defaultCompression == "zip" &&
               host::exportFormatDescription(document::ExportFormat::Png).compressions.empty(),
           "EXR lists its compressions; PNG has none");
}

// The 1.22 locator rule for a chosen path.
void testDestinationForPath() {
    const std::filesystem::path project = "/projects/shot";
    auto inside = host::exportDestinationForPath("/projects/shot/Renders/final.mov", project);
    expect(inside && inside->portability == "project-relative" &&
               inside->path == "Renders/final.mov" &&
               inside->overwrite == document::ExportOverwritePolicy::AskBeforeReplacing,
           "a path inside the project becomes project-relative");
    auto outside = host::exportDestinationForPath("/renders/final.mov", project,
                                                  document::ExportOverwritePolicy::Replace);
    expect(outside && outside->portability == "external" &&
               outside->path == "file:///renders/final.mov" &&
               outside->overwrite == document::ExportOverwritePolicy::Replace,
           "a path outside the project becomes an external file URI");
    auto sibling = host::exportDestinationForPath("/projects/shot-old/final.mov", project);
    expect(sibling && sibling->portability == "external",
           "a sibling folder with the same prefix is outside");
    auto unsaved = host::exportDestinationForPath("/renders/a b.mov", std::nullopt);
    expect(unsaved && unsaved->portability == "external" &&
               unsaved->path == "file:///renders/a%20b.mov",
           "an unsaved project always gets an external URI");
    expect(!host::exportDestinationForPath("relative.mov", project) &&
               !host::exportDestinationForPath("", project),
           "a relative or empty path is refused");
    // The helper's records resolve back to the chosen file.
    host::ExportDestinationContext context;
    context.projectFolder = project;
    document::ExportJob job;
    document::ExportOutput output;
    output.settings.format = document::ExportFormat::ProResMov;
    output.destination = *inside;
    job.outputs.push_back(output);
    expect(host::resolveDestination(job, output, context).firstPath ==
               std::filesystem::path("/projects/shot/Renders/final.mov"),
           "a project-relative record resolves to the chosen path");
}

void testPresets() {
    auto catalogue = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = linuxEncoders()});
    auto chosen = catalogue->defaultPreset();
    expect(chosen.presetId == host::kExportDefaultVideoPresetId && !chosen.usedFallback &&
               chosen.preferredAvailability.available,
           "H.264 MP4 is the default where it is available");
    // OpenH264 ships with Bloom: whatever a probe reports, H.264 MP4 stays the default and no
    // preset offers an install (owner, 2026-10-31).
    catalogue->setEncoderStatus(linuxEncoders(false, false));
    chosen = catalogue->defaultPreset();
    expect(chosen.presetId == host::kExportDefaultVideoPresetId && !chosen.usedFallback &&
               chosen.preferredAvailability.available,
           "H.264 MP4 stays the default without VA-API or a separate OpenH264 install");
    for (const auto& preset : catalogue->snapshot()->presets)
        expect(preset.platformAvailability.code !=
                       host::ExportAvailabilityCode::EncoderNotInstalled &&
                   preset.platformAvailability.remedy != host::ExportRemedy::InstallOpenH264 &&
                   preset.platformAvailability.remedy != host::ExportRemedy::LocateOpenH264,
               "no preset asks the artist to install an encoder: " + preset.id);
    document::ExportOutputSettings h264Hardware;
    h264Hardware.format = document::ExportFormat::H264Mp4;
    h264Hardware.encoder = document::ExportEncoderPreference::Hardware;
    const auto hardwareFallback = host::exportPlatformAvailability(h264Hardware, linuxEncoders());
    expect(hardwareFallback.available && hardwareFallback.reason.empty() &&
               hardwareFallback.remedy == host::ExportRemedy::None,
           "a Hardware H.264 preference without VA-API falls back to OpenH264 silently");

    auto elsewhere = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = {}});
    chosen = elsewhere->defaultPreset();
    expect(chosen.presetId == host::kExportDefaultVideoPresetId && !chosen.usedFallback &&
               chosen.preferredAvailability.code == host::ExportAvailabilityCode::Probing,
           "before the first probe the job gets H.264 MP4 and is only checking encoders");

    const auto snapshot = catalogue->snapshot();
    bool categoriesOrdered = true;
    for (std::size_t index = 1; index < snapshot->presets.size(); ++index)
        categoriesOrdered = categoriesOrdered && snapshot->presets[index - 1].category <=
                                                     snapshot->presets[index].category;
    expect(categoriesOrdered && snapshot->presets.front().id == host::kExportDefaultVideoPresetId,
           "Video comes first, then VFX / sequences, then Audio");
    for (const auto& preset : snapshot->presets)
        if (preset.capabilities.imageSequence)
            expect(preset.category == host::ExportPresetCategory::VfxSequences,
                   "image sequences live in VFX / sequences");
    for (const auto& encoders :
         {linuxEncoders(), linuxEncoders(false, false), host::ExportEncoderStatus{}}) {
        catalogue->setEncoderStatus(encoders);
        const auto id = catalogue->defaultPreset().presetId;
        const auto found =
            std::ranges::find(catalogue->snapshot()->presets, id, &host::ExportPreset::id);
        expect(found != catalogue->snapshot()->presets.end() && !found->capabilities.imageSequence,
               "the default is never an image sequence");
    }

    document::ExportOutputSettings hevc;
    hevc.format = document::ExportFormat::HevcMp4;
    hevc.encoder = document::ExportEncoderPreference::Software;
    expect(host::exportPlatformAvailability(hevc, linuxEncoders()).code ==
               host::ExportAvailabilityCode::NoQualifiedEncoder,
           "HEVC has no software encoder");
    hevc.encoder = document::ExportEncoderPreference::Automatic;
    expect(host::exportPlatformAvailability(hevc, linuxEncoders()).code ==
               host::ExportAvailabilityCode::HardwareEncoderMissing,
           "HEVC without a VA-API render node is HardwareEncoderMissing");
    expect(host::exportPlatformAvailability(hevc, linuxEncoders(true, true)).available,
           "HEVC is available with VA-API HEVC encode");

    document::ExportOutputSettings handoff;
    handoff.format = document::ExportFormat::OpenExr;
    handoff.outputColorSpaceId = "ACES2065-1";
    expect(
        catalogue->availabilityFor(handoff, document::makeBloomNeutralColorSettingsV1({})).code ==
            host::ExportAvailabilityCode::RequiresAcesConfig,
        "the ACES deliverable needs the built-in ACES config");

    // Expected preservation is what the policy approves against.
    using S = output::OutputPreservationStateV1;
    expect(host::exportPreservationFits(S::Exact, S::Approximated) &&
               !host::exportPreservationFits(S::Omitted, S::Approximated) &&
               !host::exportPreservationFits(S::Missing, S::ExternalReference),
           "a facet fits when it is at most the declared state; Missing never fits");
}

void testUserPresets() {
    const auto folder = scratch("presets");
    runtime::TaskScheduler scheduler;
    {
        auto catalogue = host::ExportPresetCatalogue::create(
            {.userDataDirectory = folder, .initialEncoders = linuxEncoders()});
        catalogue->loadUserPresets(scheduler);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto status = host::ExportUserPresetStatus::StoreUnavailable;
        while (status == host::ExportUserPresetStatus::StoreUnavailable &&
               std::chrono::steady_clock::now() < deadline) {
            document::ExportOutputSettings settings;
            settings.format = document::ExportFormat::ProResMov;
            settings.codecProfile = "4444";
            status = catalogue->saveUserPreset("Alpha master", settings);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(status == host::ExportUserPresetStatus::Accepted, "a user preset saves");
        expect(catalogue->saveUserPreset("Alpha master", {}) ==
                   host::ExportUserPresetStatus::NameTaken,
               "user preset names are unique");
        const auto id = catalogue->snapshot()->presets.back().id;
        expect(catalogue->renameUserPreset(id, "Alpha 4444") ==
                   host::ExportUserPresetStatus::Accepted,
               "a user preset renames");
        expect(catalogue->deleteUserPreset("builtin:h264-mp4") ==
                   host::ExportUserPresetStatus::BuiltInReadOnly,
               "built-ins are read-only");
        // Let the queued store writes land before the catalogue goes away.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    runtime::TaskScheduler second;
    auto reloaded = host::ExportPresetCatalogue::create(
        {.userDataDirectory = folder, .initialEncoders = linuxEncoders()});
    reloaded->loadUserPresets(second);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reloaded->snapshot()->presets.back().displayName != "Alpha 4444" &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto& last = reloaded->snapshot()->presets.back();
    expect(last.origin == host::ExportPresetOrigin::User && last.displayName == "Alpha 4444" &&
               last.settings.codecProfile == "4444" &&
               last.category == host::ExportPresetCategory::User,
           "user presets persist in the versioned store");
    second.beginShutdown();
    while (!second.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::filesystem::remove_all(folder);
}

// ---------------------------------------------------------------------------------------------
// The service against a real session.

struct Fixture {
    std::filesystem::path folder;
    host::ProjectSessionIdentitySource identity;
    host::ProjectSession session;
    runtime::NodeDefinitionRegistry registry;
    runtime::SnapshotCompiler compiler{registry};
    runtime::TaskScheduler scheduler;
    host::PublicationCoordinator publications = [] {
        auto result = host::PublicationCoordinator::create();
        if (!result)
            throw std::runtime_error("publication coordinator");
        return std::move(*result);
    }();
    platform::StagedArtifactCoordinator artifacts = [] {
        auto result = platform::StagedArtifactCoordinator::create({});
        if (!result)
            throw std::runtime_error("artifact coordinator");
        return std::move(result).takeCoordinator();
    }();
    std::shared_ptr<output::ExportResourceLedgerV1> ledger =
        std::make_shared<output::ExportResourceLedgerV1>();
    std::shared_ptr<host::GpuExportProvider> gpu;
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement =
        host::SequenceExportRetirementOwnerV1::create();
    std::shared_ptr<host::ExportPresetCatalogue> presets;
    std::unique_ptr<host::ExportQueueService> service;

    static host::ProjectSession makeSession(host::ProjectSessionIdentitySource& identity,
                                            const std::filesystem::path& folder,
                                            std::optional<document::ColorSettings> colour = {}) {
        auto format = document::CompositionFormat::create(64, 32);
        auto created =
            document::makeNewProject("Queue", "Main", core::RationalTime::fromInteger(1), *format);
        auto path = host::ProjectDisplayPath::create(folder / "queue.bloom");
        auto result = host::ProjectSession::createDecoded(
            identity,
            {.project = std::move(created.project),
             .colorSettings = colour ? *colour : document::makeBloomNeutralColorSettingsV1({}),
             .editability = host::DecodedProjectEditability::Editable,
             .displayPath = std::move(path),
             .persistedAllocatorHighWater = std::nullopt});
        if (!result)
            throw std::runtime_error("session fixture");
        return std::move(result).takeSession();
    }

    // A null root gives the provider its own worker-sampled processing root; a test may inject
    // one whose measured capacity it controls.
    static std::shared_ptr<host::GpuExportProvider>
    makeProvider(std::shared_ptr<runtime::GpuProcessingPhasePolicy> root) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        options.processingPhasePolicy = std::move(root);
        return host::GpuExportProvider::create(std::move(options));
    }

    explicit Fixture(const std::string& name, host::ExportEncoderStatus encoders = linuxEncoders(),
                     std::optional<document::ColorSettings> colour = {},
                     std::shared_ptr<runtime::GpuProcessingPhasePolicy> root = {})
        : folder(scratch(name)), session(makeSession(identity, folder, std::move(colour))),
          gpu(makeProvider(std::move(root))) {
        if (!runtime::registerBuiltInNodeDefinitions(registry))
            throw std::runtime_error("registry");
        registry.freeze();
        gpu->prepare(scheduler);
        presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = encoders});
        service = std::make_unique<host::ExportQueueService>(
            host::ExportQueueServiceDependencies{.session = session,
                                                 .scheduler = scheduler,
                                                 .compiler = compiler,
                                                 .publications = publications,
                                                 .artifacts = artifacts,
                                                 .ledger = ledger,
                                                 .gpu = gpu,
                                                 .retirement = retirement,
                                                 .presets = presets,
                                                 .limits = {},
                                                 .displayProcessor = nullptr,
                                                 .scratchDirectory = folder / "scratch",
                                                 .backend = nullptr});
        std::filesystem::create_directories(folder / "scratch");
    }
    ~Fixture() {
        service->beginShutdown(host::ExportStopCause::ApplicationQuit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!service->quiescent() && std::chrono::steady_clock::now() < deadline) {
            service->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        service.reset();
        scheduler.beginShutdown();
        retirement->beginShutdown();
        while (!scheduler.isQuiescent() || !retirement->retirementComplete())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!retirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void)gpu->shutdownAndWait(std::chrono::seconds(10));
        std::filesystem::remove_all(folder);
    }

    document::CompositionId composition() const {
        return currentSnapshot(session).project().compositions().front().id();
    }

    document::ExportJobId addJob(document::ExportOutputSettings settings, std::string path,
                                 document::ExportOverwritePolicy policy =
                                     document::ExportOverwritePolicy::AskBeforeReplacing,
                                 document::ExportRange range = {document::ExportRangeKind::Custom,
                                                                0, 2},
                                 bool importIntoAssets = false) {
        document::ExportJob job;
        job.composition = composition();
        job.render.range = range;
        document::ExportOutput output;
        output.presetId = "test";
        output.settings = std::move(settings);
        output.destination = {"project-relative", std::move(path), policy};
        output.importIntoAssets = importIntoAssets;
        job.outputs = {output};
        commands::Transaction transaction("Add export job");
        transaction.emplace<commands::AddExportJob>(job);
        const auto result = session.execute(std::move(transaction));
        if (!result.changed())
            throw std::runtime_error("add job");
        return *result.command->outputId<document::ExportJobId>(commands::kExportJobOutput);
    }

    const host::ExportJobView* view(const document::ExportJobId id) const {
        service->pump();
        const auto snapshot = service->snapshot();
        for (const auto& job : snapshot->jobs)
            if (job.id == id) {
                lastView = job;
                return &*lastView;
            }
        return nullptr;
    }

    template <typename Predicate> bool pumpUntil(Predicate&& done, int seconds = 60) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            service->pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    bool startable(const document::ExportJobId id) {
        return pumpUntil([&] { return service->canStart(id).available; }, 20);
    }

    mutable std::optional<host::ExportJobView> lastView;
};

document::ExportOutputSettings png() {
    document::ExportOutputSettings settings;
    settings.format = document::ExportFormat::Png;
    settings.includeAudio = false;
    return settings;
}

void testLifecycle() {
    Fixture f("lifecycle");
    const auto first = f.addJob(png(), "Exports/{name}.png");
    const auto second = f.addJob(png(), "Exports/second.png");
    expect(f.view(first) && f.view(first)->state == ExportJobState::Ready, "a new job is Ready");
    expect(f.service->canStart(first).code == ExportUnavailableCode::DestinationCheckPending ||
               f.service->canStart(first).available,
           "Start waits for the first destination sample: " + f.service->canStart(first).reason);
    expect(f.startable(first) && f.startable(second), "both jobs become startable");
    const document::ExportJobId both[] = {first, second};
    const auto started = f.service->start(both);
    expect(started.accepted, "start accepts both jobs");
    f.service->pump();
    const auto* secondView = f.view(second);
    expect(secondView && secondView->state == ExportJobState::Waiting,
           "only one job runs at a time; the other waits");
    expect(f.view(first)->commands[ExportCommand::EditJob].code ==
                   ExportUnavailableCode::LockedWhileRunning ||
               host::isTerminal(f.view(first)->state),
           "a running job is locked for edits");
    expect(f.pumpUntil([&] {
        const auto* a = f.view(first);
        const auto* b = f.view(second);
        return a && b && host::isTerminal(a->state) && host::isTerminal(b->state);
    }),
           "both jobs finish");
    const auto* done = f.view(first);
    expect(done && (done->state == ExportJobState::Done ||
                    done->state == ExportJobState::DoneWithWarnings),
           "the first job succeeds");
    if (done && done->failure)
        std::cerr << "  failure: " << done->failure->reason << '\n';
    expect(done && done->outputs.front().verification &&
               done->outputs.front().verification->artifacts.publishedArtifacts == 3,
           "the sequence verifies three published frames");
    expect(std::filesystem::exists(f.folder / "Exports/Main.0002.png"),
           "the last frame is on disk");
    const auto events = f.service->events(0);
    std::size_t succeeded = 0;
    bool drained = false;
    for (const auto& event : events.events) {
        succeeded += std::holds_alternative<host::ExportJobSucceeded>(event.payload) ? 1 : 0;
        drained = drained || std::holds_alternative<host::ExportQueueDrained>(event.payload);
    }
    expect(succeeded == 2 && drained, "terminal events report both jobs and the drain");
    expect(f.service->log(first, 0).entries.size() >= 2, "the run log is readable");

    // Re-running the files this session published needs no confirmation.
    expect(f.pumpUntil([&] { return f.service->canStart(first).available; }, 20),
           "a re-run over this session's own files needs no confirmation");

    // Retry starts a fresh run.
    const auto retried = f.service->retry(first);
    expect(retried.accepted, "retry accepts a finished job");
    f.service->pump();
    expect(f.view(first)->run && f.view(first)->run->run > 1, "retry starts a new run");
    // Stop cancels an active job.
    expect(f.pumpUntil([&] { return host::isTerminal(f.view(first)->state); }), "retry finishes");
    const auto longJob =
        f.addJob(png(), "Exports/long.png", document::ExportOverwritePolicy::Replace,
                 {document::ExportRangeKind::Composition, 0, 0});
    expect(f.startable(longJob), "the long job becomes startable");
    const document::ExportJobId longIds[] = {longJob};
    expect(f.service->start(longIds).accepted, "start the long job");
    const auto stopped = f.service->stop(longJob);
    expect(stopped.accepted, "stop accepts an active job");
    expect(f.pumpUntil([&] { return host::isTerminal(f.view(longJob)->state); }),
           "the stopped job ends");
    expect(f.view(longJob)->state == ExportJobState::Stopped &&
               f.view(longJob)->stopCause == host::ExportStopCause::User,
           "stop ends in Stopped with the user cause");
}

// A processing root that measures no usable host memory and has no ceiling to wait for refuses
// the export's frames. The job ends with a readable failure (or waits and stops on request); a
// refusal before any frame exists never crashes the service.
void testZeroHostCapacityRefusal() {
    const auto root = std::make_shared<runtime::GpuProcessingPhasePolicy>();
    runtime::MemoryBudgetState zero;
    zero.machine.availableBytes = 0;
    zero.process.source = runtime::ProcessMemorySource::OtherHostOnly;
    zero.process.status = runtime::ProcessMemoryStatus::Unsupported;
    zero.process.sampledAt = std::chrono::steady_clock::now();
    zero.sampleSerial = 1;
    zero.processSampleFresh = true;
    root->applyHostSample(zero, 0, zero.process.sampledAt);
    expect(root->snapshot().reservations.hostReady &&
               root->snapshot().reservations.hostCapacityBytes == 0,
           "the injected root measures zero usable host memory");
    Fixture f("zero-host", linuxEncoders(), {}, root);
    const auto job = f.addJob(png(), "Exports/zero.png", document::ExportOverwritePolicy::Replace);
    expect(f.startable(job), "the zero-capacity job becomes startable");
    const document::ExportJobId ids[] = {job};
    expect(f.service->start(ids).accepted, "start the zero-capacity job");
    const bool ended = f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }, 20);
    if (!ended) {
        expect(f.service->stop(job).accepted, "a waiting zero-capacity job stops on request");
        expect(f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }),
               "the stopped zero-capacity job ends");
    }
    const auto* view = f.view(job);
    expect(view && view->state != ExportJobState::Done &&
               view->state != ExportJobState::DoneWithWarnings,
           "no frame is published without usable memory");
    expect(view && (view->state == ExportJobState::Stopped ||
                    (view->failure && !view->failure->reason.empty())),
           "the refusal is a readable failure");
    if (view && view->failure)
        std::cerr << "  zero-capacity failure: " << view->failure->reason << '\n';
}

void testReplaceConfirmation() {
    Fixture f("replace");
    std::filesystem::create_directories(f.folder / "Exports");
    {
        std::ofstream(f.folder / "Exports/still.png") << "not a png";
    }
    const auto job =
        f.addJob(png(), "Exports/still.png", document::ExportOverwritePolicy::AskBeforeReplacing,
                 {document::ExportRangeKind::Frame, 0, 0});
    expect(f.pumpUntil(
               [&] {
                   return f.service->canStart(job).code ==
                          ExportUnavailableCode::NeedsReplaceConfirmation;
               },
               20),
           "an existing destination needs confirmation under AskBeforeReplacing");
    const auto* view = f.view(job);
    expect(view && view->outputs.front().commands[ExportCommand::ConfirmReplace].available,
           "confirm replace is offered");
    expect(f.service->confirmReplace(job, view->outputs.front().id).accepted,
           "confirm replace records the sample");
    expect(f.service->canStart(job).available, "a confirmed job can start");
    const document::ExportJobId ids[] = {job};
    expect(f.service->start(ids).accepted, "start after confirmation");
    expect(f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }), "replace finishes");
    expect(f.view(job)->state != ExportJobState::Failed, "the confirmed file is replaced");

    const auto refusing =
        f.addJob(png(), "Exports/still.png", document::ExportOverwritePolicy::Refuse,
                 {document::ExportRangeKind::Frame, 0, 0});
    expect(f.pumpUntil(
               [&] {
                   return f.service->canStart(refusing).code ==
                          ExportUnavailableCode::PreflightBlocked;
               },
               20),
           "Refuse blocks Start while the destination exists");
}

// ADR 0035: every operating system exports. A computer whose only media provider is the bundled
// FFmpeg worker (Windows today: no Media Foundation provider, no VA-API) refuses nothing for its
// platform, and no format FFmpeg serves is reported unavailable: H.264 comes from the bundled
// OpenH264.
host::ExportEncoderStatus ffmpegOnlyEncoders() {
    host::ExportEncoderStatus status;
    status.probed = true;
    status.mediaWorker = true;
    status.h264Software = output::H264EncoderV1::OpenH264;
    status.openh264 = host::ExportEncoderStatus::OpenH264::Installed;
    return status;
}

void testEveryPlatformExports() {
    const auto catalogue = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = ffmpegOnlyEncoders()});
    for (const auto& preset : catalogue->snapshot()->presets) {
        const auto format = preset.settings.format;
        const bool needsHevc =
            format == document::ExportFormat::HevcMov || format == document::ExportFormat::HevcMp4;
        expect(preset.platformAvailability.code !=
                       host::ExportAvailabilityCode::WorkerUnavailable &&
                   preset.platformAvailability.reason.find("Linux") == std::string::npos,
               "no preset is refused for its operating system: " + preset.id);
        if (!needsHevc && format != document::ExportFormat::Dpx)
            expect(preset.platformAvailability.available &&
                       preset.platformAvailability.reason.empty(),
                   "the FFmpeg worker serves the preset with no unavailable reason: " + preset.id);
    }
    Fixture f("every-platform", ffmpegOnlyEncoders());
    const auto job = f.addJob(png(), "Exports/x.png");
    expect(f.startable(job), "Start is available: " + f.service->canStart(job).reason);
    expect(f.service->availability(ExportCommand::StartAll).available, "Start All is available");
    host::ExportTransientJobRequest transient;
    transient.composition = f.composition();
    transient.range = {document::ExportRangeKind::Frame, 0, 0};
    transient.settings = png();
    transient.destination = {"external", fileUri(f.folder / "t.png"),
                             document::ExportOverwritePolicy::Replace};
    const auto submitted = f.service->submitTransient(transient);
    expect(submitted.accepted,
           "a transient job is accepted on every platform: " + submitted.availability.reason);
    if (submitted.accepted && submitted.job) {
        expect(f.pumpUntil([&] { return host::isTerminal(f.view(*submitted.job)->state); }),
               "the transient job finishes");
        (void)f.service->acknowledgeTransient(*submitted.job);
    }
}

void testTransient() {
    Fixture f("transient");
    host::ExportTransientJobRequest request;
    request.composition = f.composition();
    request.range = {document::ExportRangeKind::Frame, 0, 0};
    request.settings = png();
    request.destination = {"external", fileUri(f.folder / "capture.png"),
                           document::ExportOverwritePolicy::Replace};
    const auto submitted = f.service->submitTransient(request);
    expect(submitted.accepted && submitted.job, "a transient job is accepted");
    if (!submitted.job)
        return;
    const auto id = *submitted.job;
    expect(f.view(id) && f.view(id)->transient, "the snapshot marks it transient");
    expect(currentSnapshot(f.session).project().exportJobs().empty(),
           "a transient job is never saved in the project");
    expect(f.view(id)->run && f.view(id)->run->group.isValid(),
           "a transient job owns and groups its tasks");
    expect(f.pumpUntil([&] { return f.view(id) && host::isTerminal(f.view(id)->state); }),
           "the transient job finishes");
    expect(std::filesystem::exists(f.folder / "capture.png") &&
               f.view(id)->state != ExportJobState::Failed,
           "the transient job publishes its still");
    expect(f.service->acknowledgeTransient(id), "the result is acknowledged");
    expect(f.view(id) == nullptr, "an acknowledged terminal transient job leaves the snapshot");

    // Never silent: an existing destination under AskBeforeReplacing is refused at run time.
    request.destination.overwrite = document::ExportOverwritePolicy::AskBeforeReplacing;
    const auto ask = f.service->submitTransient(request);
    expect(ask.accepted && ask.job, "an asking transient job is accepted");
    if (ask.job) {
        expect(f.pumpUntil([&] { return host::isTerminal(f.view(*ask.job)->state); }),
               "the asking transient job ends");
        expect(f.view(*ask.job)->state == ExportJobState::Failed &&
                   f.view(*ask.job)->failure->code == host::ExportFailureCode::PublicationConflict,
               "an existing destination is never replaced silently");
    }
    // Stop by id.
    request.range = {document::ExportRangeKind::Custom, 0, 23};
    request.settings.format = document::ExportFormat::Png;
    request.destination = {"external", fileUri(f.folder / "long.png"),
                           document::ExportOverwritePolicy::Replace};
    request.settings.naming.pattern = "<base>.####.<ext>";
    const auto running = f.service->submitTransient(request);
    if (running.job) {
        expect(f.service->stop(*running.job).accepted, "a transient job stops by id");
        expect(f.pumpUntil([&] {
            const auto* v = f.view(*running.job);
            return v && (host::isTerminal(v->state) || v->state == ExportJobState::Ready);
        }),
               "the stopped transient job ends");
    }
    const auto missing =
        f.service->submitTransient({.composition = document::CompositionId::fromRaw(99),
                                    .range = {},
                                    .settings = png(),
                                    .destination = {},
                                    .name = {},
                                    .presetId = {}});
    expect(!missing.accepted &&
               missing.availability.code == ExportUnavailableCode::CompositionMissing,
           "a transient job needs an existing composition");
}

// OCIO colour-space names are free text. A working space such as "Linear Rec.709 (sRGB)" (spaces
// and parentheses) used to fail every frame's analysis before encoding; stills, sequences and
// movies all export with it now.
void testFreeTextWorkingSpace(const std::string& workingSpace) {
    const auto revision = color::ocioBuiltInContentRevision(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
    if (!revision) {
        expect(false, "the ACES config is available");
        return;
    }
    auto colour = document::makeBloomNeutralColorSettingsV1({});
    colour.processColorSpaceId = workingSpace;
    colour.ocioConfig.locator =
        document::BuiltInOcioConfigLocator{std::string(color::kAcesCgV1ConfigUri)};
    colour.ocioConfig.expectedRevision.digest = *revision;
    Fixture f("working-space", linuxEncoders(), colour);
    std::vector<std::pair<document::ExportJobId, std::string>> jobs;
    const auto add = [&](document::ExportFormat format, std::string profile, std::string path,
                         document::ExportRange range) {
        document::ExportOutputSettings settings;
        settings.format = format;
        settings.codecProfile = std::move(profile);
        settings.includeAudio = false;
        jobs.emplace_back(f.addJob(settings, path, document::ExportOverwritePolicy::Replace, range),
                          path);
    };
    add(document::ExportFormat::Png, {}, "Exports/still.png",
        {document::ExportRangeKind::Frame, 0, 0});
    add(document::ExportFormat::OpenExr, {}, "Exports/still.exr",
        {document::ExportRangeKind::Frame, 0, 0});
    add(document::ExportFormat::Tiff, {}, "Exports/still.tiff",
        {document::ExportRangeKind::Frame, 0, 0});
    add(document::ExportFormat::ProResMov, "hq", "Exports/movie.mov",
        {document::ExportRangeKind::Custom, 0, 1});
    std::vector<document::ExportJobId> ids;
    for (const auto& [id, path] : jobs) {
        expect(f.startable(id), workingSpace + ": " + path + " becomes startable");
        ids.push_back(id);
    }
    expect(f.service->start(ids).accepted, workingSpace + ": start");
    expect(f.pumpUntil(
               [&] {
                   return std::ranges::all_of(
                       ids, [&](auto id) { return host::isTerminal(f.view(id)->state); });
               },
               180),
           workingSpace + ": every job finishes");
    for (const auto& [id, path] : jobs) {
        const auto* view = f.view(id);
        if (view && view->failure)
            std::cerr << "  " << path << ": " << view->failure->reason << '\n';
        expect(view && view->state != ExportJobState::Failed &&
                   std::filesystem::exists(f.folder / path),
               workingSpace + ": " + path + " exports with a free-text working space");
        if (view && view->failure)
            expect(view->failure->reason.find("stage=") == std::string::npos &&
                       view->failure->reason.find("code=") == std::string::npos,
                   "failure text carries no stage or code numbers");
    }
}

// End to end: a movie through the media worker, reopened by the worker and checked for its
// container, colour tags, frame count and digest.
void testVideoEndToEnd(const document::ExportFormat format,
                       const host::ExportEncoderStatus encoders, const std::string& name) {
    Fixture f(name, encoders);
    document::ExportOutputSettings settings;
    settings.format = format;
    settings.codecProfile = format == document::ExportFormat::ProResMov ? "hq" : "high";
    settings.includeAudio = false;
    const auto extension = std::string(host::exportFormatExtension(format));
    const auto job = f.addJob(settings, "Exports/{name}." + extension,
                              document::ExportOverwritePolicy::AskBeforeReplacing,
                              {document::ExportRangeKind::Composition, 0, 0});
    expect(f.startable(job), name + ": the movie job becomes startable");
    const document::ExportJobId ids[] = {job};
    expect(f.service->start(ids).accepted, name + ": start");
    expect(f.pumpUntil([&] { return host::isTerminal(f.view(job)->state); }, 120),
           name + ": the movie finishes");
    const auto* view = f.view(job);
    if (view && view->failure)
        std::cerr << "  " << name << " failure: " << view->failure->reason << '\n';
    expect(view && view->state != ExportJobState::Failed, name + ": the movie publishes");
    if (!view || !view->outputs.front().verification)
        return;
    const auto& verification = *view->outputs.front().verification;
    // A failed run carries a partial verification: an absent check is a failed expectation,
    // never a dereference of the end iterator.
    const auto passed = [&](host::ExportCheck what) {
        const auto found =
            std::ranges::find(verification.checks, what, &host::ExportCheckResult::check);
        return found != verification.checks.end() &&
               found->outcome == host::ExportCheckOutcome::Passed;
    };
    expect(passed(host::ExportCheck::Reopen), name + ": reopened by the worker");
    expect(passed(host::ExportCheck::FrameCount), name + ": frame count matches the range");
    expect(passed(host::ExportCheck::Container), name + ": the container and codec match");
    expect(passed(host::ExportCheck::ColourTags),
           name + ": the color tags match the output color intent");
    expect(verification.artifacts.digest.has_value() &&
               std::filesystem::exists(f.folder / ("Exports/Main." + extension)),
           name + ": the published file has its digest");
}

} // namespace

int main() {
    try {
        testResolveDestination();
        testScript();
        testPresets();
        testDefaults();
        testFormatTokens();
        testDestinationForPath();
        testUserPresets();
        testLifecycle();
        testZeroHostCapacityRefusal();
        testReplaceConfirmation();
        testEveryPlatformExports();
        testTransient();
        testVideoEndToEnd(document::ExportFormat::ProResMov, linuxEncoders(), "prores-mov");
        testFreeTextWorkingSpace("Linear Rec.709 (sRGB)");
        testFreeTextWorkingSpace("Linear P3-D65");
        // H.264 MP4 end to end on the bundled OpenH264, with nothing installed.
        testVideoEndToEnd(document::ExportFormat::H264Mp4, linuxEncoders(), "h264-mp4");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
