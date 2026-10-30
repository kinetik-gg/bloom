// The stdio MCP server's composition root (ADR 0034): open the headless session, construct the
// headless export queue over it and the shared server core from src/mcp, then serve stdio.
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/cache_directory.hpp>
#include <bloom/platform/owner_only_secret.hpp>
#include <bloom/platform/user_data_directory.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

// A private scratch folder for publication's bridging files (never a destination).
std::filesystem::path scratchFolder() {
    std::array<std::byte, 8> random{};
    static_cast<void>(platform::fillSecureRandom(random));
    std::string name = "bloom-mcp-";
    constexpr std::string_view digits = "0123456789abcdef";
    for (const auto byte : random) {
        const auto value = std::to_integer<unsigned>(byte);
        name.push_back(digits[value >> 4U]);
        name.push_back(digits[value & 0xFU]);
    }
    return std::filesystem::temp_directory_path() / name;
}

int serve(scripting::Session& session) {
    runtime::TaskScheduler scheduler;
    // One server-lifetime GPU final-render provider, bootstrapped once on the scheduler's worker
    // and shared by render, capture and the headless export queue; no per-request bootstrap. The
    // packaged GPU shader tools come from this executable's own definitions (never PATH), and the
    // media context follows the opened project's asset base. A device or tool refusal keeps the
    // CPU reference path. The evaluator is declared first so it outlives the provider.
    runtime::CpuCompositionEvaluator mediaEvaluator;
    if (!session.displayPath().empty())
        mediaEvaluator.setAssetBaseDirectory(session.displayPath().parent_path());
    runtime::GpuProcessFrameEvaluatorOptions gpuOptions;
    gpuOptions.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    gpuOptions.mediaContextProvider = [&mediaEvaluator] {
        return runtime::GpuSceneMediaContext::fromEvaluator(mediaEvaluator);
    };
    auto gpu = host::GpuExportProvider::create(std::move(gpuOptions));
    gpu->prepare(scheduler);

    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto retirement = host::SequenceExportRetirementOwnerV1::create();
    host::ExportEncoderStatus encoders;
    encoders.exportPlatform = host::exportPlatformSupported();
    auto presets = host::ExportPresetCatalogue::create(
        {.userDataDirectory = platform::userDataDirectory(), .initialEncoders = encoders});
    presets->refreshEncoders(scheduler);
    presets->loadUserPresets(scheduler);
    const auto scratch = scratchFolder();
    std::error_code ignored;
    std::filesystem::create_directories(scratch, ignored);
    const auto makeQueue = [&] {
        return std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
            .session = *session.hostSession(),
            .scheduler = scheduler,
            .compiler = compiler,
            .publications = *session.publicationCoordinator(),
            .artifacts = *session.artifactCoordinator(),
            .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
            .gpu = gpu,
            .retirement = retirement,
            .presets = presets,
            .limits = {},
            .displayProcessor = nullptr,
            .scratchDirectory = scratch,
            .backend = nullptr});
    };
    const auto stopQueue = [](host::ExportQueueService& queue, const host::ExportStopCause cause) {
        queue.beginShutdown(cause);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!queue.quiescent() && std::chrono::steady_clock::now() < deadline) {
            queue.pump();
            std::this_thread::sleep_for(2ms);
        }
    };
    auto queue = makeQueue();
    std::uint64_t queueGeneration = 1;
    mcp::McpServerCore* corePointer = nullptr;
    // project.save, save-as, open, new and close on the headless session (ADR 0034 item 9 as
    // amended). This thread owns the session and the queue, and the core handles one request at a
    // time, so the replacement happens between requests' reads.
    const auto lifecycle = [&](const mcp::McpProjectRequest& request,
                               const std::function<void(mcp::McpProjectResult)>& done) {
        mcp::McpProjectResult result;
        result.projectName = session.snapshot().project().name();
        const auto refuse = [&](const mcp::McpUiRefusal refusal, std::string message) {
            result.refusal = refusal;
            result.message = std::move(message);
            done(result);
        };
        using Kind = mcp::McpProjectRequestKind;
        if (request.kind == Kind::Save || request.kind == Kind::SaveAs) {
            if (request.kind == Kind::Save && session.displayPath().empty())
                return refuse(mcp::McpUiRefusal::NoPath,
                              result.projectName +
                                  " has never been saved; use project.save-as with a path.");
            if (request.kind == Kind::SaveAs) {
                if (request.path.extension() != ".bloom" ||
                    !std::filesystem::is_directory(request.path.parent_path(), ignored))
                    return refuse(mcp::McpUiRefusal::PathInvalid,
                                  "Save As needs a .bloom path in a folder that exists.");
                if (std::filesystem::exists(request.path, ignored) && !request.replace)
                    return refuse(mcp::McpUiRefusal::DestinationExists,
                                  request.path.filename().string() +
                                      " already exists; pass replace:true to replace it.");
            }
            const auto saved =
                request.kind == Kind::Save ? session.save() : session.saveAs(request.path);
            if (!saved.succeeded())
                return refuse(saved.status == scripting::SessionSaveStatus::ReadOnly
                                  ? mcp::McpUiRefusal::ReadOnly
                                  : mcp::McpUiRefusal::Failed,
                              saved.message.empty() ? "The project could not be saved."
                                                    : saved.message);
            result.path = session.displayPath();
            result.message = "Saved " + result.projectName + ".";
            return done(result);
        }
        const auto state = session.state();
        if (state.dirty.value_or(false) && !request.discardChanges)
            return refuse(mcp::McpUiRefusal::UnsavedChanges,
                          result.projectName +
                              " has unsaved changes; save it first or pass discardChanges:true.");
        if (queue->activeJobCount() != 0 && !request.stopJobs)
            return refuse(mcp::McpUiRefusal::JobsRunning,
                          "Export jobs are running; wait for them or pass stopJobs:true.");
        if (request.kind == Kind::Open &&
            (request.path.extension() != ".bloom" ||
             !std::filesystem::is_regular_file(request.path, ignored)))
            return refuse(mcp::McpUiRefusal::PathInvalid,
                          "Open needs the path of an existing .bloom file.");
        auto opened = request.kind == Kind::Open ? scripting::Session::open(request.path)
                                                 : scripting::Session::createNew();
        if (!opened)
            return refuse(mcp::McpUiRefusal::Failed, opened.diagnostic().message.empty()
                                                         ? "The project could not be opened."
                                                         : opened.diagnostic().message);
        const auto swap = [&] {
            stopQueue(*queue, host::ExportStopCause::ProjectClosing);
            queue.reset();
            session.adopt(std::move(opened).takeSession());
            mediaEvaluator.setAssetBaseDirectory(session.displayPath().empty()
                                                     ? std::filesystem::path{}
                                                     : session.displayPath().parent_path());
            queue = makeQueue();
            ++queueGeneration;
        };
        if (corePointer != nullptr)
            corePointer->replaceHeadlessProject(swap);
        else
            swap();
        result.path = session.displayPath();
        result.projectName = session.snapshot().project().name();
        result.message = request.kind == Kind::Open  ? "Opened " + result.projectName + "."
                         : request.kind == Kind::New ? "Created a new project."
                                                     : "Closed the project; an empty project "
                                                       "is open.";
        done(result);
    };
    {
        const auto captures = platform::userCacheDirectory();
        mcp::McpServerCore core(
            {.session = std::ref(session),
             // This thread owns the headless queue: queue calls run inline.
             .exports = {.run =
                             [&queue, &queueGeneration](const mcp::McpExportAccess::Work& work,
                                                        const std::stop_token& stop) {
                                 if (stop.stop_requested())
                                     return false;
                                 work(queue.get(), queueGeneration);
                                 return true;
                             },
                         .callerIsOwner = true},
             .scheduler = scheduler,
             .gpuProvider = gpu,
             .captureDirectory = captures ? *captures / "mcp-captures" : std::filesystem::path{},
             .ui = {.revealComposition = {},
                    .projectLifecycle = lifecycle,
                    .setSelection = {},
                    .activateComposition = {},
                    .transport = {},
                    .setViewer = {},
                    .setColorSettings = {}},
             .serverVersion = BLOOM_VERSION,
             .limits = mcp::kMcpMessageLimits});
        corePointer = &core;
        mcp::serveStdio(core, std::cin, std::cout, {});
        corePointer = nullptr;
    }
    stopQueue(*queue, host::ExportStopCause::ApplicationQuit);
    queue.reset();
    // Headless teardown runs on this thread, never a UI event loop: retire the provider and the
    // encoder retirement owner while the scheduler that serves them is still alive.
    retirement->beginShutdown();
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!retirement->retirementComplete() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    while (!retirement->collect() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    static_cast<void>(gpu->shutdownAndWait(10s));
    std::filesystem::remove_all(scratch, ignored);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 1 && !(argc == 3 && std::string_view(argv[1]) == "--project")) {
            std::cerr << "usage: bloom-mcp [--project <file>]\n";
            return 2;
        }
        auto opened = argc == 3 ? bloom::scripting::Session::open(argv[2])
                                : bloom::scripting::Session::createNew();
        if (!opened) {
            std::cerr << opened.diagnostic().message << '\n';
            return 2;
        }
        const auto session = std::move(opened).takeSession();
        return serve(*session);
    } catch (const std::exception& error) {
        std::cerr << "bloom-mcp: " << error.what() << '\n';
        return 1;
    }
}
