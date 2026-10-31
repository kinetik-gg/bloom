// The shared MCP core's tool behaviour driven directly through McpServerCore::handle(), without a
// transport: the export queue's absence, and (live host) the session and UI seams of
// live_ui_port.hpp.

#include <bloom/host/export_queue.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
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

    // No export queue unless `exports` binds one (as in a window that has not created one).
    explicit Live(mcp::McpLiveUiPort ui, std::optional<mcp::McpExportAccess> exports = {},
                  std::filesystem::path captures = {}) {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     {.renderSource = session->renderSourceSnapshot(),
                      .context = session->operationContext(),
                      .viewerDisplay = {},
                      .viewerView = {},
                      .projectPath = std::nullopt,
                      .readOnly = false});
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(host),
            .exports = exports ? std::move(*exports)
                               : mcp::McpExportAccess{.run =
                                                          [](const mcp::McpExportAccess::Work& work,
                                                             const std::stop_token&) {
                                                              work(nullptr, 1);
                                                              return true;
                                                          },
                                                      .callerIsOwner = false},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = std::move(captures),
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
             {"project.new", "{}"},
             {"project.set-color-settings", R"({"config":"aces"})"}}) {
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
    std::optional<document::ColorSettings> colour;
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
            },
        .setColorSettings =
            [&](const document::ColorSettings& settings) {
                colour = settings;
                return mcp::McpUiResult{};
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
    // Colour settings are resolved by the core and applied by the application through the port.
    result = server.call("project.set-color-settings", R"({"config":"aces"})");
    expect(contains(result, R"("code":"updated")") && colour.has_value(),
           "project.set-color-settings reaches the port: " + result);
    expect(contains(result, R"("workingSpace":"ACEScg")") &&
               contains(result, R"("workingSpaceLabel":"ACEScg")"),
           "project.set-color-settings reads back the working space id and label: " + result);
    result = server.call("project.set-color-settings", R"({"config":"neutral"})");
    expect(contains(result, R"("workingSpace":"lin_rec709_scene")") &&
               contains(result, R"json("workingSpaceLabel":"Linear Rec.709 (sRGB)")json"),
           "Bloom Neutral reads back lin_rec709_scene labelled Linear Rec.709 (sRGB): " + result);
    result = server.call("events", R"({"sinceRevision":0})");
    expect(contains(result, R"("kind":"project.saved")"), "events list project.saved: " + result);
}

// The id after the first `"job":` of a response.
std::uint64_t jobOf(const std::string& response) {
    const auto at = response.find(R"("job":)");
    return at == std::string::npos ? 0 : std::stoull(response.substr(at + 6));
}

// A finished transient render/export/capture job stays readable after its call returned and the
// queue retired it (export.log, export.status, export.wait), and capture carries its job and log.
void retiredTransientJobs() {
    const auto captures =
        std::filesystem::temp_directory_path() /
        ("bloom-mcp-core-tools-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(captures);
    auto fake = std::make_shared<host::testing::FakeExportRunBackend>();
    std::unique_ptr<host::ExportQueueService> queue;
    std::uint64_t generation = 1;
    mcp::McpExportAccess exports{
        .run =
            [&](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                work(queue.get(), generation);
                return true;
            },
        .callerIsOwner = true};
    runtime::NodeDefinitionRegistry registry;
    const runtime::SnapshotCompiler compiler{registry};
    Live server({}, std::move(exports), captures);
    // The queue borrows the server's session and scheduler: it goes first.
    struct Release final {
        std::unique_ptr<host::ExportQueueService>& queue;
        std::filesystem::path folder;
        ~Release() {
            queue.reset();
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } release{queue, captures};
    host::ExportEncoderStatus encoders;
    encoders.probed = true;
    queue = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
        .session = *server.session->hostSession(),
        .scheduler = server.scheduler,
        .compiler = compiler,
        .publications = *server.session->publicationCoordinator(),
        .artifacts = *server.session->artifactCoordinator(),
        .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
        .gpu = nullptr,
        .retirement = nullptr,
        .presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = encoders}),
        .limits = {},
        .displayProcessor = nullptr,
        .scratchDirectory = {},
        .backend = fake});

    fake->push({.outcome = host::testing::FakeExportScript::Outcome::FailRendering});
    const auto destination = (captures / "failed.png").string();
    const auto failed = server.call("render", R"({"composition":1,"frame":0,)"
                                              R"("preset":"PngRgba8SrgbV1","destination":")" +
                                                  destination + R"("})");
    const auto job = jobOf(failed);
    expect(contains(failed, R"("succeeded":false)") && job != 0 && contains(failed, R"("log":[)"),
           "a failed render carries its job and log: " + failed);
    const auto id = std::to_string(job);
    const auto log = server.call("export.log", R"({"job":)" + id + "}");
    expect(contains(log, "export.fake.started") && !contains(log, R"("lastSequence":0)"),
           "export.log reads the finished transient job after the call returned: " + log);
    const auto after = server.call("export.log", R"({"job":)" + id + R"(,"afterSequence":1})");
    expect(contains(after, R"("entries":[)") && !contains(after, R"("sequence":1,)"),
           "export.log honors afterSequence on a retired job: " + after);
    auto status = server.call("export.status", "{}");
    expect(contains(status, R"("recentTransientJobs":[{"id":)" + id) &&
               contains(status, R"("transient":true)"),
           "export.status lists the retired transient job: " + status);
    // The refusal codes are named by enumerator, not by position: ADR 0035 retired value 23, and
    // the codes after it must keep their own names.
    expect(contains(status, R"("code":"DestinationCheckPending")") &&
               !contains(status, R"("code":"Unknown")"),
           "export.status names the codes after the retired gap correctly: " + status);
    const auto wait = server.call("export.wait", R"({"job":)" + id + R"(,"timeoutMs":1000})");
    expect(contains(wait, R"("finished":true)"), "export.wait finds the retired job: " + wait);

    fake->push({.writeDestinations = true});
    const auto capture = server.call("capture", R"({"composition":1,"frame":0})");
    const auto captureJob = jobOf(capture);
    expect(contains(capture, R"("isError":false)") && captureJob != 0 && captureJob != job &&
               contains(capture, R"("log":[{)"),
           "a capture carries its transient job and log: " + capture);
    expect(contains(server.call("export.log", R"({"job":)" + std::to_string(captureJob) + "}"),
                    R"("entries":[{)"),
           "export.log reads the capture's retired job");

    // 33 more retire the oldest: the ring keeps the newest 32.
    for (int index = 0; index < 33; ++index) {
        fake->push({.outcome = host::testing::FakeExportScript::Outcome::FailRendering});
        static_cast<void>(server.call("render", R"({"composition":1,"frame":0,)"
                                                R"("preset":"PngRgba8SrgbV1","destination":")" +
                                                    destination + R"("})"));
    }
    expect(contains(server.call("export.log", R"({"job":)" + id + "}"), R"("entries":[])"),
           "the ring is bounded: the oldest retired job is released");
    // A replaced project (a new queue generation) releases every retired job.
    generation = 2;
    status = server.call("export.status", "{}");
    expect(contains(status, R"("recentTransientJobs":[])"),
           "a new queue generation releases the retired jobs: " + status);
}

} // namespace

// Document 1.30 read-back: query kind=layers, kind=keyframes and kind=enums carry the easing
// equations, their parameters and the shy / nameFollowsSource layer flags the new operations write.
std::string firstMatch(const std::string& text, const std::string& pattern) {
    std::smatch match;
    return std::regex_search(text, match, std::regex(pattern)) ? match[1].str() : std::string{};
}

template <typename Server> std::string revisionOf(Server& server) {
    return firstMatch(server.call("query", R"({"kind":"project"})"), R"re("revision":(\d+))re");
}

void animationReadBack() {
    Headless server;
    const auto transact = [&](const std::string& operations) {
        return server.call("transact", R"({"expectedRevision":)" + revisionOf(server) +
                                           R"(,"operations":[)" + operations + "]}");
    };
    auto result = transact(R"({"op":"bloom.layer.add-solid","args":{"composition":1,)"
                           R"("name":"Solid","color":[1,0,0,1]}})");
    expect(contains(result, R"("isError":false)"), "the solid fixture applies: " + result);
    const auto layers = server.call("query", R"({"kind":"layers","composition":1})");
    const auto layer = firstMatch(layers, R"re("records":\[\{"id":(\d+))re");
    expect(!layer.empty() && contains(layers, R"("shy":false)") &&
               contains(layers, R"("nameFollowsSource":false)"),
           "layers read back with their presentation flags: " + layers);
    const auto nodes = server.call("query", R"({"kind":"nodes","composition":1})");
    const auto opacity = firstMatch(nodes, R"re("opacity":(\d+))re");
    expect(!opacity.empty(), "the layer's opacity parameter is addressable: " + nodes);
    for (const auto* at : {R"(0,"value":0.0)", R"(12,"value":1.0)"})
        static_cast<void>(transact(
            R"({"op":"bloom.animation.set-keyframe-at-time-for-parameter","args":{"composition":1,)"
            R"("parameter":)" +
            opacity + R"(,"time":)" + at + "}}"));
    auto keyframes = server.call("query", R"({"kind":"keyframes","composition":1})");
    const auto curve = firstMatch(keyframes, R"re("curve":(\d+))re");
    const auto key = firstMatch(keyframes, R"re("keys":\[\{"id":(\d+))re");
    expect(!curve.empty() && !key.empty() && contains(keyframes, R"("interpolation":"linear")"),
           "keyframes read back with their interpolation token: " + keyframes);
    result = transact(R"({"op":"bloom.animation.set-keyframes-easing","args":{"composition":1,)"
                      R"("keys":[[)" +
                      curve + "," + key +
                      R"(]],"interpolation":"elastic","ease":"out","amplitude":2.0}},)"
                      R"({"op":"bloom.layer.set-shy","args":{"composition":1,"layer":)" +
                      layer + R"(,"shy":true}})");
    keyframes = server.call("query", R"({"kind":"keyframes","composition":1})");
    expect(contains(keyframes, R"("interpolation":"elastic")") &&
               contains(keyframes, R"("interpolationId":12)") &&
               contains(keyframes, R"("ease":"out")") && contains(keyframes, R"("amplitude":2)"),
           "set-keyframes-easing reads back mode and parameters: " + result + keyframes);
    expect(contains(server.call("query", R"({"kind":"layers","composition":1})"), R"("shy":true)"),
           "set-shy reads back");
    const auto enums = server.call("query", R"({"kind":"enums"})");
    expect(contains(enums, R"("token":"bounce")") && contains(enums, R"("label":"Bezier")") &&
               contains(enums, R"("token":"in-out")") &&
               contains(enums, R"("label":"Easy Ease Out")"),
           "the interpolation, ease and Easy Ease tables are listed: " + enums);
}

int main() {
    try {
        exportWithoutQueue();
        unboundSeams();
        liveSeams();
        retiredTransientJobs();
        animationReadBack();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "core tools passed\n";
    return failures == 0 ? 0 : 1;
}
