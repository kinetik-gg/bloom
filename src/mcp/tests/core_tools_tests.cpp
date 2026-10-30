// The shared MCP core's tool behaviour driven directly through McpServerCore::handle(), without a
// transport: the export queue's absence, and (live host) the session and UI seams of
// live_ui_port.hpp.

#include <bloom/mcp/server_core.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

struct Headless final {
    std::unique_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::move(created).takeSession();
    }();
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    explicit Headless(mcp::McpLiveUiPort ui = {}) {
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(*session),
            // No export queue is bound, as in a desktop window that has not created one.
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = true},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = {},
            .ui = std::move(ui),
            .serverVersion = "test",
            .limits = mcp::kMcpMessageLimits});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Headless() {
        core.reset();
        scheduler.beginShutdown();
    }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        const auto handled = core->handle(*protocol, message, control);
        return handled.response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

void exportWithoutQueue() {
    Headless server;
    const auto status = server.call("export.status", "{}");
    expect(contains(status, "Export isn't available in this window yet."),
           "a missing export queue says so plainly: " + status);
    expect(contains(status, "bloom.mcp.export-unavailable"),
           "a missing export queue keeps its typed code: " + status);
    expect(!contains(status, "while a project is opening"), "no false reason: " + status);
}

// The desktop host: a live session the test thread owns, with a fake McpLiveUiPort.
struct Live final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    scripting::LiveSessionHost host;
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    explicit Live(mcp::McpLiveUiPort ui) {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     {.renderSource = session->renderSourceSnapshot(),
                      .context = session->operationContext(),
                      .viewerDisplay = {},
                      .viewerView = {},
                      .projectPath = std::nullopt,
                      .readOnly = false});
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(host),
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = false},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = {},
            .ui = std::move(ui),
            .serverVersion = "test",
            .limits = mcp::kMcpMessageLimits});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Live() {
        core.reset();
        scheduler.beginShutdown();
    }
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        return core->handle(*protocol, message, control).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

void unboundSeams() {
    Live server({});
    for (const auto& [tool, arguments] : std::vector<std::pair<std::string, std::string>>{
             {"editor.reveal", R"({"composition":1})"},
             {"selection.set", R"({"composition":1,"layers":[]})"},
             {"composition.activate", R"({"composition":1})"},
             {"transport", R"({"action":"play"})"},
             {"viewer.set", R"({"zoom":"fit"})"},
             {"project.new", "{}"}}) {
        const auto result = server.call(tool, arguments);
        expect(contains(result, "bloom.mcp.ui-unbound"), tool + " unbound is typed: " + result);
    }
}

void liveSeams() {
    std::optional<core::RationalTime> revealed;
    document::CompositionId revealedComposition;
    mcp::McpSelectionRequest selection;
    mcp::McpTransportRequest transport;
    mcp::McpViewerRequest viewer;
    std::optional<document::CompositionId> activated;
    std::vector<mcp::McpProjectRequest> lifecycle;
    std::jthread later;
    mcp::McpLiveUiPort port{
        .revealComposition =
            [&](const document::CompositionId composition,
                const std::optional<core::RationalTime> time) {
                revealedComposition = composition;
                revealed = time;
                return composition.value() == 1;
            },
        .projectLifecycle =
            [&](mcp::McpProjectRequest request, std::function<void(mcp::McpProjectResult)> done) {
                lifecycle.push_back(request);
                if (request.kind == mcp::McpProjectRequestKind::New) {
                    done({.refusal = mcp::McpUiRefusal::UnsavedChanges,
                          .message = "Untitled has unsaved changes.",
                          .path = {},
                          .projectName = "Untitled"});
                    return;
                }
                // Asynchronous, as the desktop binding is: finish from another thread later.
                later = std::jthread([done, path = request.path] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    done({.refusal = mcp::McpUiRefusal::None,
                          .message = "Saved.",
                          .path = path,
                          .projectName = "Study"});
                });
            },
        .setSelection =
            [&](const mcp::McpSelectionRequest& request) {
                selection = request;
                return mcp::McpUiResult{};
            },
        .activateComposition =
            [&](const document::CompositionId composition) {
                activated = composition;
                return mcp::McpUiResult{};
            },
        .transport =
            [&](const mcp::McpTransportRequest& request) {
                transport = request;
                return mcp::McpUiResult{};
            },
        .setViewer =
            [&](const mcp::McpViewerRequest& request) {
                viewer = request;
                return mcp::McpUiResult{.refusal = mcp::McpUiRefusal::None, .message = {}};
            }};
    Live server(std::move(port));

    // One time contract: an integer is a whole frame at the composition's rate (24 fps), and
    // [numerator, denominator] is exact seconds.
    auto result = server.call("editor.reveal", R"({"composition":1,"time":12})");
    expect(contains(result, R"("succeeded":true)") && revealed &&
               *revealed == *core::RationalTime::create(1, 2),
           "editor.reveal reads 12 as frame 12: " + result);
    result = server.call("editor.reveal", R"({"composition":1,"time":[3,2]})");
    expect(revealed && *revealed == *core::RationalTime::create(3, 2),
           "editor.reveal reads a pair as seconds: " + result);
    result = server.call("editor.reveal", R"({"composition":9})");
    expect(contains(result, "bloom.mcp.unknown-target"), "an unknown composition: " + result);

    result = server.call("selection.set", R"({"composition":1,"layers":[4,5],"mode":"add"})");
    expect(contains(result, R"("succeeded":true)") && selection.layers.size() == 2 &&
               selection.mode == mcp::McpSelectionMode::Add,
           "selection.set reaches the port: " + result);
    result = server.call("composition.activate", R"({"composition":1})");
    expect(activated && activated->value() == 1, "composition.activate: " + result);
    result = server.call("transport", R"({"action":"seek","time":24})");
    expect(transport.action == mcp::McpTransportAction::Seek && transport.time &&
               *transport.time == core::RationalTime::fromInteger(1),
           "transport seeks 24 frames of the active composition: " + result);
    result = server.call("viewer.set", R"({"zoom":"fit","channel":"alpha","exposure":1.5})");
    expect(viewer.zoom == mcp::McpViewerZoom::Fit &&
               viewer.channel == mcp::McpViewerChannel::Alpha && viewer.exposure == 1.5 &&
               !viewer.display,
           "viewer.set passes only the given fields: " + result);

    result = server.call("project.open", R"({"path":"relative.bloom"})");
    expect(contains(result, "bloom.mcp.path-invalid") && lifecycle.empty(),
           "a relative path never reaches the application: " + result);
    result = server.call("project.new", "{}");
    expect(contains(result, "bloom.mcp.unsaved-changes") && contains(result, "Untitled"),
           "project.new over unsaved changes is refused by name: " + result);
#ifdef _WIN32
    const std::string path = R"(C:\\study.bloom)";
#else
    const std::string path = "/tmp/study.bloom";
#endif
    result = server.call("project.save-as", R"({"path":")" + path + R"(","replace":true})");
    expect(contains(result, R"("succeeded":true)") && contains(result, "project.saved") &&
               lifecycle.size() == 2 && lifecycle.back().replace,
           "project.save-as waits for the asynchronous binding: " + result);
    result = server.call("events", R"({"sinceRevision":0})");
    expect(contains(result, R"("kind":"project.saved")"), "events list project.saved: " + result);
}

} // namespace

int main() {
    try {
        exportWithoutQueue();
        unboundSeams();
        liveSeams();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "core tools passed\n";
    return failures == 0 ? 0 : 1;
}
