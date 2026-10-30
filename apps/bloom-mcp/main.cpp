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
#include <iostream>
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
    {
        host::ExportQueueService queue(
            {.session = *session.hostSession(),
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
        {
            const auto captures = platform::userCacheDirectory();
            mcp::McpServerCore core(
                {.session = std::ref(session),
                 // This thread owns the headless queue: queue calls run inline.
                 .exports = {.run =
                                 [&queue](const mcp::McpExportAccess::Work& work,
                                          const std::stop_token& stop) {
                                     if (stop.stop_requested())
                                         return false;
                                     work(&queue, 1);
                                     return true;
                                 },
                             .callerIsOwner = true},
                 .scheduler = scheduler,
                 .gpuProvider = gpu,
                 .captureDirectory =
                     captures ? *captures / "mcp-captures" : std::filesystem::path{},
                 .serverVersion = BLOOM_VERSION,
                 .limits = mcp::kMcpMessageLimits});
            mcp::serveStdio(core, std::cin, std::cout, {});
        }
        queue.beginShutdown(host::ExportStopCause::ApplicationQuit);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!queue.quiescent() && std::chrono::steady_clock::now() < deadline) {
            queue.pump();
            std::this_thread::sleep_for(2ms);
        }
    }
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
