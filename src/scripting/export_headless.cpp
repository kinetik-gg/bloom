// The export queue for a headless session (bloom-cli, bloom-mcp): it owns the scheduler, compiler,
// GPU export provider and encoder retirement owner the queue borrows and retires them in order.
#include <bloom/scripting/export.hpp>

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/user_data_directory.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>

#include <iostream>
#include <thread>

namespace bloom::scripting {
namespace {
runtime::TaskSchedulerConfig schedulerConfig() {
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return config;
}
} // namespace

struct HeadlessExportQueue::State {
    runtime::TaskScheduler scheduler{schedulerConfig()};
    runtime::SnapshotCompiler compiler{document::builtInNodeDefinitions()};
    runtime::CpuCompositionEvaluator media;
    std::shared_ptr<host::GpuExportProvider> gpu;
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement =
        host::SequenceExportRetirementOwnerV1::create();
    std::shared_ptr<host::ExportPresetCatalogue> presets;
    std::filesystem::path scratch;
    std::unique_ptr<host::ExportQueueService> service;
    std::optional<ExportCalls> calls;
};

HeadlessExportQueue::HeadlessExportQueue(Session& session) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    const auto projectFolder = session.displayPath().empty()
                                   ? std::filesystem::path{}
                                   : std::filesystem::absolute(session.displayPath()).parent_path();
    if (!projectFolder.empty())
        s.media.setAssetBaseDirectory(projectFolder);
    runtime::GpuProcessFrameEvaluatorOptions gpuOptions;
    gpuOptions.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    gpuOptions.mediaContextProvider = [state = state_.get()] {
        return runtime::GpuSceneMediaContext::fromEvaluator(state->media);
    };
    s.gpu = host::GpuExportProvider::create(std::move(gpuOptions));
    s.gpu->prepare(s.scheduler);
    host::ExportEncoderStatus encoders;
    s.presets = host::ExportPresetCatalogue::create(
        {.userDataDirectory = platform::userDataDirectory(), .initialEncoders = encoders});
    s.presets->refreshEncoders(s.scheduler);
    s.presets->loadUserPresets(s.scheduler);
    s.scratch = std::filesystem::temp_directory_path() / "bloom-export-scratch";
    std::error_code error;
    std::filesystem::create_directories(s.scratch, error);
    s.service = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
        .session = *session.hostSession(),
        .scheduler = s.scheduler,
        .compiler = s.compiler,
        .publications = *session.publicationCoordinator(),
        .artifacts = *session.artifactCoordinator(),
        .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
        .gpu = s.gpu,
        .retirement = s.retirement,
        .presets = s.presets,
        .limits = {},
        .displayProcessor = nullptr,
        .scratchDirectory = s.scratch,
        .backend = nullptr});
    // Encoders answer on a worker before anything can start.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!s.presets->snapshot()->encoders.probed && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    s.calls.emplace(*s.service);
    s.service->pump();
}

HeadlessExportQueue::~HeadlessExportQueue() {
    auto& s = *state_;
    s.service->beginShutdown(host::ExportStopCause::ApplicationQuit);
    while (!s.service->quiescent()) {
        s.service->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    s.calls.reset();
    s.service.reset();
    while (!s.gpu->shutdownAndWait(std::chrono::seconds(10)))
        std::cerr << "export retirement pending; provider/media/scheduler retained\n";
    s.scheduler.beginShutdown();
    s.retirement->beginShutdown();
    while (!s.scheduler.isQuiescent() || !s.retirement->retirementComplete())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (!s.retirement->collect())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

host::ExportQueueService& HeadlessExportQueue::service() { return *state_->service; }
ExportCalls& HeadlessExportQueue::calls() { return *state_->calls; }

} // namespace bloom::scripting
