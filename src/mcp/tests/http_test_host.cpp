// A headless McpServerService for the Streamable HTTP conformance test (ADR 0034, section 10). It
// binds a headless session to a LiveSessionHost exactly as the desktop adapter does, gives the
// service a headless export queue through the provider seam, and plays the owner (UI) thread on
// main: pump, drain one commit per slice, pump the queue.
//
// usage: bloom_mcp_http_test_host --port <n> --folder <dir> [--header-timeout-ms <n>]
// Prints "READY <port>" or "REFUSED <reason>", then follows commands on stdin, one per line:
// start, stop, regenerate, quit (end of input also quits). The token is in <dir>/mcp/token.
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/mcp/server_service.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

struct Commands final {
    std::mutex mutex;
    std::deque<std::string> lines;
    std::atomic_bool closed = false;
};

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    std::filesystem::path folder;
    mcp::McpServerLimits limits;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string_view flag = argv[index];
        const std::string value = argv[index + 1];
        if (flag == "--port")
            port = static_cast<std::uint16_t>(std::stoul(value));
        else if (flag == "--folder")
            folder = value;
        else if (flag == "--header-timeout-ms")
            limits.http.headerTimeout = std::chrono::milliseconds(std::stoul(value));
    }
    if (port == 0 || folder.empty()) {
        std::cerr << "usage: bloom_mcp_http_test_host --port <n> --folder <dir>\n";
        return 2;
    }
    auto created = scripting::Session::createNew();
    if (!created)
        return 2;
    const auto session = std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    auto events = std::make_shared<scripting::EventStream>();
    const auto observer = session->commandStack()->addObserver(
        [events](const commands::CommandEvent& event) { events->publish(event); });
    scripting::LiveSessionHost live;
    const auto mirror = [&] {
        auto context = session->operationContext();
        context.layerGeometry = {};
        return scripting::LiveSessionState{.renderSource = session->renderSourceSnapshot(),
                                           .context = std::move(context),
                                           .viewerDisplay = {},
                                           .viewerView = {},
                                           .projectPath = std::nullopt,
                                           .readOnly = false};
    };
    live.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                 mirror());
    const scripting::LiveSessionCommitter committer{
        .execute =
            [&](commands::Transaction transaction) {
                auto result = session->execute(std::move(transaction));
                live.publish(mirror());
                return result;
            },
        .history =
            [&](const bool redo) {
                auto result = redo ? session->redo() : session->undo();
                live.publish(mirror());
                return result;
            },
        .gestureOpen = [] { return false; },
        .revision = [&] { return session->snapshot().revision(); }};

    runtime::TaskScheduler scheduler;
    auto gpu = [] {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        return host::GpuExportProvider::create(std::move(options));
    }();
    gpu->prepare(scheduler);
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto retirement = host::SequenceExportRetirementOwnerV1::create();
    host::ExportEncoderStatus encoders;
    auto presets = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = encoders});
    presets->refreshEncoders(scheduler);
    std::filesystem::create_directories(folder / "scratch");
    auto queue = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
        .session = *session->hostSession(),
        .scheduler = scheduler,
        .compiler = compiler,
        .publications = *session->publicationCoordinator(),
        .artifacts = *session->artifactCoordinator(),
        .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
        .gpu = gpu,
        .retirement = retirement,
        .presets = presets,
        .limits = {},
        .displayProcessor = nullptr,
        .scratchDirectory = folder / "scratch",
        .backend = nullptr});

    int exitCode = 0;
    {
        mcp::McpServerService service({.live = live,
                                       .exportQueue = [&] { return queue.get(); },
                                       .scheduler = scheduler,
                                       .tokenFile = folder / "mcp" / "token",
                                       .captureDirectory = folder / "captures",
                                       .serverVersion = "test",
                                       .preferences = {.port = port, .autoStart = true},
                                       .limits = limits});
        service.requestAutoStart();
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            service.pump();
            const auto snapshot = service.snapshot();
            if (snapshot->state == mcp::McpServerState::Running || snapshot->failure)
                break;
            std::this_thread::sleep_for(2ms);
        }
        const auto started = service.snapshot();
        if (started->state == mcp::McpServerState::Running) {
            std::cout << "READY " << port << std::endl;
        } else {
            std::cout << "REFUSED " << (started->failure ? started->failure->reason : "timeout")
                      << std::endl;
            exitCode = 3;
        }
        Commands commands;
        std::thread reader([&] {
            std::string line;
            while (std::getline(std::cin, line)) {
                const std::lock_guard lock(commands.mutex);
                commands.lines.push_back(line);
            }
            commands.closed = true;
        });
        bool quit = exitCode != 0;
        while (!quit) {
            std::deque<std::string> lines;
            {
                const std::lock_guard lock(commands.mutex);
                lines.swap(commands.lines);
            }
            for (const auto& line : lines) {
                if (line == "quit")
                    quit = true;
                else if (line == "stop")
                    static_cast<void>(service.stop());
                else if (line == "start")
                    static_cast<void>(service.start());
                else if (line == "regenerate")
                    static_cast<void>(service.regenerateToken());
                std::cout << "DONE " << line << std::endl;
            }
            quit = quit || commands.closed;
            service.pump();
            queue->pump();
            static_cast<void>(live.drainOne(committer));
            std::this_thread::sleep_for(1ms);
        }
        service.beginShutdown();
        const auto stopDeadline = std::chrono::steady_clock::now() + 30s;
        while (!service.quiescent() && std::chrono::steady_clock::now() < stopDeadline) {
            service.pump();
            queue->pump();
            static_cast<void>(live.drainOne(committer));
            std::this_thread::sleep_for(1ms);
        }
        // The reader blocks on stdin until the test closes it.
        reader.detach();
    }
    queue->beginShutdown(host::ExportStopCause::ApplicationQuit);
    const auto queueDeadline = std::chrono::steady_clock::now() + 30s;
    while (!queue->quiescent() && std::chrono::steady_clock::now() < queueDeadline) {
        queue->pump();
        std::this_thread::sleep_for(2ms);
    }
    queue.reset();
    live.revoke(scripting::LiveSessionAvailability::ShuttingDown);
    session->commandStack()->removeObserver(observer);
    scheduler.beginShutdown();
    retirement->beginShutdown();
    while (!scheduler.isQuiescent() || !retirement->retirementComplete())
        std::this_thread::sleep_for(1ms);
    while (!retirement->collect())
        std::this_thread::sleep_for(1ms);
    static_cast<void>(gpu->shutdownAndWait(10s));
    std::cout.flush();
    std::_Exit(exitCode);
}
