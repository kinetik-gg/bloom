// bloom.mcp.server-service (ADR 0034, section 10): the in-app server's lifecycle, typed refusals,
// token, activity, events, client configuration and shutdown, against a headless live session.
// The test thread is the owner (UI) thread.
#include <bloom/mcp/server_service.hpp>
#include <bloom/mcp/testing/server_service_driver.hpp>
#include <bloom/platform/owner_only_secret.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <variant>

namespace {

using namespace bloom;
using namespace bloom::mcp;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

fs::path scratch(const std::string& name) {
    std::array<std::byte, 6> random{};
    static_cast<void>(platform::fillSecureRandom(random));
    std::string suffix;
    for (const auto byte : random)
        suffix += std::to_string(std::to_integer<unsigned>(byte) % 10U);
    const auto folder = fs::temp_directory_path() / ("bloom-mcp-service-" + name + "-" + suffix);
    fs::create_directories(folder);
    return folder;
}

// A headless live session: the test thread plays the UI thread that owns it.
struct Live final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    scripting::LiveSessionHost host;
    Live() {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     {.renderSource = session->renderSourceSnapshot(),
                      .context = session->operationContext(),
                      .viewerDisplay = {},
                      .viewerView = {},
                      .projectPath = std::nullopt,
                      .readOnly = false});
    }
};

struct Fixture final {
    fs::path folder;
    runtime::TaskScheduler scheduler;
    Live live;
    std::unique_ptr<McpServerService> service;

    explicit Fixture(const std::string& name, McpServerPreferences preferences = {},
                     const bool tokenFolder = true)
        : folder(scratch(name)) {
        service = std::make_unique<McpServerService>(McpServerServiceDependencies{
            .live = live.host,
            .exportQueue = {},
            .scheduler = scheduler,
            .tokenFile = tokenFolder ? folder / "mcp" / "token" : fs::path{},
            .captureDirectory = folder / "captures",
            .serverVersion = "test",
            .preferences = preferences,
            .limits = {}});
    }
    ~Fixture() {
        service->beginShutdown();
        pumpUntil([&] { return service->quiescent(); });
        service.reset();
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent())
            std::this_thread::sleep_for(1ms);
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    template <typename Predicate> bool pumpUntil(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            service->pump();
            std::this_thread::sleep_for(2ms);
        }
        return predicate();
    }
    bool tokenReady() {
        return pumpUntil([&] { return service->snapshot()->token.state == McpTokenState::Ready; });
    }
    McpServerSnapshot snapshot() const { return *service->snapshot(); }
};

// Reason-string style (ADR 0034, section 7): sentence case, a full stop, no internal vocabulary,
// about one line.
void expectReason(const std::string& reason, const std::string& where) {
    const bool sentence = !reason.empty() && reason.front() >= 'A' && reason.front() <= 'Z' &&
                          reason.back() == '.' && reason.size() <= 90;
    bool vocabulary = false;
    for (const std::string_view word :
         {"socket", "bind", "token file", "errno", "EADDR", "WSA", "Port6767", "_", "::"})
        vocabulary = vocabulary || reason.find(word) != std::string::npos;
    expect(sentence && !vocabulary, where + ": reason style \"" + reason + "\"");
}

void lifecycle() {
    const auto port = testing::McpServerServiceDriver::freePort(42000);
    Fixture f("lifecycle", {.port = port, .autoStart = false});
    expect(f.snapshot().state == McpServerState::Stopped, "off until started");
    expect(f.tokenReady(), "the token loads on the blocking-I/O executor");
    const auto masked = f.snapshot().token.masked;
    const auto started = f.service->start();
    expect(started.accepted && f.snapshot().state == McpServerState::Running, "start binds");
    expect(f.snapshot().endpoint &&
               f.snapshot().endpoint->url == "http://127.0.0.1:" + std::to_string(port) + "/mcp",
           "the endpoint is literal loopback");
    const auto again = f.service->start();
    expect(!again.accepted && again.availability.code == McpUnavailableCode::AlreadyRunning,
           "start while running is AlreadyRunning");
    const auto setPort = f.service->setPort(port == 50000 ? 50001 : 50000);
    expect(!setPort.accepted && setPort.availability.code == McpUnavailableCode::ServerRunning &&
               setPort.availability.field == McpField::Port,
           "setPort while running is ServerRunning on the Port field");
    const auto outOfRangeRunning = f.service->setPort(80);
    expect(outOfRangeRunning.availability.code == McpUnavailableCode::ServerRunning,
           "setPort refuses ServerRunning before the range");
    const auto stopped = f.service->stop();
    expect(stopped.accepted, "stop is accepted");
    const auto stopping = f.snapshot().state;
    expect(stopping == McpServerState::Stopping || stopping == McpServerState::Stopped,
           "stop goes through Stopping");
    expect(f.pumpUntil([&] { return f.snapshot().state == McpServerState::Stopped; }),
           "Stopped once the workers are idle");
    const auto notRunning = f.service->stop();
    expect(!notRunning.accepted && notRunning.availability.code == McpUnavailableCode::NotRunning,
           "stop while stopped is NotRunning");
    expect(f.service->start().accepted, "restart rebinds the same port immediately");
    expect(f.service->stop().accepted, "stop again");
    expect(f.pumpUntil([&] { return f.snapshot().state == McpServerState::Stopped; }),
           "stopped again");
    expect(f.snapshot().token.masked == masked, "the token survives a restart");

    // A taken port: PortInUse naming the port, no other port tried, kept as the failure.
    auto holder = testing::McpServerServiceDriver::occupyPort(port);
    expect(holder != nullptr, "the test holds the port");
    const auto refused = f.service->start();
    expect(!refused.accepted && refused.availability.code == McpUnavailableCode::PortInUse &&
               refused.availability.port == port && refused.availability.field == McpField::Port,
           "a taken port is PortInUse on the Port field naming the port");
    expect(refused.availability.reason == "Port " + std::to_string(port) +
                                              " is in use. Choose another port or stop the other "
                                              "program.",
           "the PortInUse reason");
    expectReason(refused.availability.reason, "PortInUse");
    const auto failed = f.snapshot();
    expect(failed.state == McpServerState::Stopped && failed.failure &&
               failed.failure->code == McpServerFailureCode::PortInUse &&
               failed.failure->port == port && !failed.endpoint,
           "the refusal stays as the failure and no other port is bound");
    const auto events = f.service->events(0);
    bool startRefused = false;
    for (const auto& event : events.events)
        startRefused = startRefused || std::holds_alternative<McpServerStartRefused>(event.payload);
    expect(startRefused, "McpServerStartRefused is published");
    holder.reset();
    expect(f.service->setPort(port).accepted && !f.snapshot().failure,
           "setPort clears a failure about the old port");
}

void portValidation() {
    Fixture f("port");
    expect(!validateMcpPort(6767) && !validateMcpPort(1024) && !validateMcpPort(65535),
           "in-range ports validate");
    for (const std::uint32_t value : {0U, 1U, 80U, 1023U, 65536U, 70000U, 4294967295U}) {
        const auto live = validateMcpPort(value);
        const auto command = f.service->setPort(value);
        expect(live.has_value() && !command.accepted && command.availability.code == live->code &&
                   command.availability.field == live->field &&
                   command.availability.port == live->port &&
                   command.availability.reason == live->reason &&
                   live->code == McpUnavailableCode::PortOutOfRange &&
                   live->field == McpField::Port && live->port == value,
               "setPort and validateMcpPort agree for " + std::to_string(value));
        expectReason(live->reason, "PortOutOfRange");
    }
    expect(validateMcpPort(80)->reason ==
               "Port 80 can't be used. Choose a port from 1024 to 65535.",
           "the out-of-range reason");
    const auto before = f.snapshot().preferencesRevision;
    expect(f.service->setPort(7000).accepted && f.snapshot().preferences.port == 7000 &&
               f.snapshot().preferencesRevision > before,
           "the port is remembered as a preference");
    expect(f.service->setAutoStart(true).accepted && f.snapshot().preferences.autoStart,
           "auto-start is a preference");
}

void autoStartWaitsForToken() {
    const auto port = testing::McpServerServiceDriver::freePort(43000);
    Fixture f("autostart", {.port = port, .autoStart = true});
    testing::McpServerServiceDriver driver(*f.service);
    expect(f.tokenReady(), "token ready");
    const auto token = f.service->revealToken().token;
    driver.setToken(McpTokenState::Loading);
    f.service->pump();
    f.service->requestAutoStart();
    expect(f.snapshot().state == McpServerState::Stopped &&
               f.snapshot().commands[McpServerCommand::Start].code ==
                   McpUnavailableCode::TokenLoading,
           "auto-start waits while the token loads");
    driver.setToken(McpTokenState::Ready, token);
    expect(f.pumpUntil([&] { return f.snapshot().state == McpServerState::Running; }),
           "auto-start runs once the token is ready");
}

void refusalsForEveryState() {
    Fixture f("refusals", {}, false);
    const auto s = f.snapshot();
    expect(s.token.state == McpTokenState::Unavailable &&
               s.token.reason == "Bloom can't find a folder to keep the access token in.",
           "no token folder makes the token Unavailable with the reason");
    const auto start = s.commands[McpServerCommand::Start];
    expect(!start && start.code == McpUnavailableCode::TokenUnavailable &&
               start.field == McpField::Token,
           "start needs the token");
    expect(s.commands[McpServerCommand::RevealToken].code == McpUnavailableCode::TokenUnavailable,
           "reveal needs the token");
    expect(f.service->revealToken().token.empty(), "no token is revealed");
    const auto placeholder = f.service->clientConfig(McpClientKind::ClaudeCode,
                                                     McpTokenDisclosure::EnvironmentPlaceholder);
    expect(placeholder.availability.available &&
               placeholder.text.find("${BLOOM_MCP_TOKEN}") != std::string::npos,
           "the placeholder config is always available");
    expect(!f.service->clientConfig(McpClientKind::ClaudeCode, McpTokenDisclosure::IncludeToken)
                .availability,
           "the token config needs the token");
    for (const auto& entry : s.commands.entries) {
        if (!entry.available)
            expectReason(entry.reason, "command refusal");
    }
    f.service->beginShutdown();
    for (const auto& entry : f.snapshot().commands.entries) {
        expect(!entry.available && entry.code == McpUnavailableCode::ShuttingDown,
               "every command is refused after shutdown begins");
        expectReason(entry.reason, "ShuttingDown");
    }
    expect(f.service->setAutoStart(true).availability.code == McpUnavailableCode::ShuttingDown,
           "setAutoStart is refused while shutting down");
}

void tokenAndActivity() {
    Fixture f("token");
    expect(f.tokenReady(), "token ready");
    const auto reveal = f.service->revealToken();
    const auto token = reveal.token;
    expect(reveal.availability && tokenWellFormed(token), "reveal returns a well-formed token");
    const auto masked = f.snapshot().token.masked;
    expect(masked.starts_with("bloom_") && masked.ends_with(token.substr(token.size() - 4)) &&
               masked.find(token.substr(6, 10)) == std::string::npos,
           "the snapshot shows the token masked");
    const auto read = platform::readOwnerOnlySecret(f.folder / "mcp" / "token", 256);
    expect(read.status == platform::OwnerOnlySecretStatus::Ok && read.value == token,
           "the token is stored owner-only");

    testing::McpServerServiceDriver driver(*f.service);
    const auto titled = driver.connectClient(
        {.name = "agent", .version = "1", .title = "Agent X", .protocolVersion = "2025-11-25"});
    const auto named = driver.connectClient(
        {.name = std::string(80, 'n'), .version = "2", .title = {}, .protocolVersion = {}});
    driver.recordToolCall(titled, McpTool::Transact, McpCallOutcome::Succeeded, std::nullopt, 12ms);
    driver.recordRejection(McpHttpRejection::Unauthorized);
    driver.recordRejection(McpHttpRejection::Unauthorized);
    f.service->pump();
    const auto clients = f.snapshot().clients;
    expect(clients.size() == 2 && clients[0].lastTool == McpTool::Transact,
           "clients and last tool");
    const auto config =
        f.service->clientConfig(McpClientKind::McpJson, McpTokenDisclosure::IncludeToken);
    expect(config.text.find(token) != std::string::npos, "the token config carries the token");

    // Regenerate: every client is disconnected, the token changes.
    expect(f.service->regenerateToken().accepted, "regenerate is accepted");
    expect(f.pumpUntil([&] {
        return f.snapshot().token.state == McpTokenState::Ready && f.snapshot().clients.empty();
    }),
           "regenerate disconnects every client");
    const auto renewed = f.service->revealToken().token;
    expect(tokenWellFormed(renewed) && renewed != token, "a new token");
    bool regenerated = false;
    bool tokenCause = false;
    for (const auto& event : f.service->events(0).events) {
        regenerated = regenerated || std::holds_alternative<McpTokenRegenerated>(event.payload);
        if (const auto* gone = std::get_if<McpClientDisconnected>(&event.payload))
            tokenCause = tokenCause || gone->cause == McpDisconnectCause::TokenRegenerated;
    }
    expect(regenerated && tokenCause, "McpTokenRegenerated and TokenRegenerated disconnects");

    // Activity: monotonic ids from 1, labels captured at record time, folding, no token.
    const auto batch = f.service->activity(0);
    std::uint64_t previous = 0;
    bool monotonic = !batch.entries.empty() && batch.entries.front().id == 1;
    std::size_t rejected = 0;
    bool labelKept = false;
    for (const auto& entry : batch.entries) {
        monotonic = monotonic && entry.id > previous;
        previous = entry.id;
        if (entry.kind == McpActivityKind::RequestRejected) {
            ++rejected;
            expect(entry.repeat == 2, "identical rejections within a second fold");
        }
        if (entry.kind == McpActivityKind::ToolCall)
            labelKept = entry.clientLabel == "Agent X";
        if (entry.client == named)
            expect(entry.clientLabel.size() <= 64 && entry.clientLabel.starts_with("nnn"),
                   "the label falls back to the name, capped at 64 bytes");
        for (const auto& secret : {token, renewed})
            expect(entry.summary.find(secret) == std::string::npos &&
                       entry.clientLabel.find(secret) == std::string::npos,
                   "no activity entry contains the token");
    }
    expect(monotonic && rejected == 1 && labelKept, "activity ids, folding and labels");
    expect(f.snapshot().lastActivityId == batch.lastId, "the snapshot carries the newest id");
    for (const auto& event : f.service->events(0).events) {
        if (const auto* connected = std::get_if<McpClientConnected>(&event.payload))
            expect(connected->client.info.name.find(token) == std::string::npos,
                   "no token in events");
    }
    const auto later = f.service->activity(batch.lastId);
    expect(later.entries.empty() && !later.gap, "incremental reads return nothing new");
}

void activityGap() {
    Fixture f("gap");
    testing::McpServerServiceDriver driver(*f.service);
    const auto client =
        driver.connectClient({.name = "a", .version = "1", .title = {}, .protocolVersion = {}});
    for (int index = 0; index < 600; ++index)
        driver.recordToolCall(client, McpTool::Query);
    const auto batch = f.service->activity(0);
    expect(batch.gap && batch.entries.size() == 512,
           "the ring keeps 512 entries and reports a gap");
    const auto tail = f.service->activity(batch.lastId - 3);
    expect(!tail.gap && tail.entries.size() == 3, "reading inside the ring has no gap");
    driver.disconnectClient(client, McpDisconnectCause::Expired);
    f.service->pump();
    const auto last = f.service->activity(batch.lastId);
    expect(!last.entries.empty() && last.entries.back().clientLabel == "a",
           "a disconnected client's lines stay labelled");
}

void clientConfigTexts() {
    McpEndpoint endpoint;
    endpoint.port = 6767;
    endpoint.url = "http://127.0.0.1:6767/mcp";
    const std::string token = "bloom_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";
    for (const std::string& credential : {token, std::string("${BLOOM_MCP_TOKEN}")}) {
        expect(clientConfigText(McpClientKind::ClaudeCode, endpoint, credential) ==
                   "claude mcp add --transport http bloom http://127.0.0.1:6767/mcp --header "
                   "\"Authorization: Bearer " +
                       credential + "\"",
               "Claude Code text");
        expect(
            clientConfigText(McpClientKind::McpJson, endpoint, credential) ==
                R"({"mcpServers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer )" +
                    credential + R"("}}}})",
            "MCP JSON text");
        expect(
            clientConfigText(McpClientKind::VsCode, endpoint, credential) ==
                R"({"servers":{"bloom":{"type":"http","url":"http://127.0.0.1:6767/mcp","headers":{"Authorization":"Bearer )" +
                    credential + R"("}}}})",
            "VS Code text");
        expect(clientConfigText(McpClientKind::Generic, endpoint, credential) ==
                   "http://127.0.0.1:6767/mcp\nAuthorization: Bearer " + credential,
               "Other client text");
    }
    Fixture f("config", {.port = 7123, .autoStart = false});
    const auto stopped =
        f.service->clientConfig(McpClientKind::Generic, McpTokenDisclosure::EnvironmentPlaceholder);
    expect(stopped.text.starts_with("http://127.0.0.1:7123/mcp") && stopped.title == "Other client",
           "the config uses the configured port while stopped");
}

void listenerFailure() {
    const auto port = testing::McpServerServiceDriver::freePort(44000);
    Fixture f("failure", {.port = port, .autoStart = false});
    expect(f.tokenReady(), "token ready");
    expect(f.service->start().accepted, "started");
    testing::McpServerServiceDriver(*f.service).failListener();
    expect(f.pumpUntil([&] { return f.snapshot().state == McpServerState::Failed; }),
           "a listener failure goes to Failed");
    expect(f.snapshot().failure &&
               f.snapshot().failure->code == McpServerFailureCode::ListenerFailed,
           "with the failure");
    expectReason(f.snapshot().failure->reason, "ListenerFailed");
    expect(f.snapshot().commands[McpServerCommand::Start].available, "Start is available again");
    expect(f.service->setPort(port).accepted && f.snapshot().state == McpServerState::Stopped &&
               !f.snapshot().failure,
           "setPort from Failed goes to Stopped");
}

void shutdownQuiesces() {
    const auto port = testing::McpServerServiceDriver::freePort(45000);
    Fixture f("shutdown", {.port = port, .autoStart = false});
    expect(f.tokenReady() && f.service->start().accepted, "running");
    f.service->beginShutdown();
    expect(f.pumpUntil([&] { return f.service->quiescent(); }), "shutdown reaches quiescent()");
    expect(f.snapshot().state == McpServerState::Stopped, "and Stopped");
    bool cause = false;
    for (const auto& event : f.service->events(0).events) {
        if (const auto* stopped = std::get_if<McpServerStopped>(&event.payload))
            cause = cause || stopped->cause == McpStopCause::Shutdown;
    }
    expect(cause, "McpServerStopped carries Shutdown");
}

void subscriptionWakes() {
    Fixture f("wake");
    auto subscription = f.service->subscribe();
    const auto observed = subscription.serial();
    testing::McpServerServiceDriver driver(*f.service);
    std::thread record([&] {
        std::this_thread::sleep_for(20ms);
        driver.recordRejection(McpHttpRejection::HostNotAllowed);
    });
    const auto changed = subscription.waitForChange(observed, {});
    record.join();
    expect(changed && *changed != observed, "recorded facts wake the waiter");
    const auto before = f.snapshot().revision;
    f.service->pump();
    expect(f.snapshot().revision > before, "pump publishes them");
    const auto same = f.snapshot().revision;
    f.service->pump();
    expect(f.snapshot().revision == same, "a pump with nothing new publishes nothing");
}

} // namespace

int main() {
    try {
        lifecycle();
        portValidation();
        autoStartWaitsForToken();
        refusalsForEveryState();
        tokenAndActivity();
        activityGap();
        clientConfigTexts();
        listenerFailure();
        shutdownQuiesces();
        subscriptionWakes();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "server service: lifecycle, refusals, token, activity, events, config and "
                     "shutdown passed\n";
    return failures == 0 ? 0 : 1;
}
