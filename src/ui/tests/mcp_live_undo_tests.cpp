// bloom.ui.mcp-live-undo (ADR 0034, section 10): an MCP transact over Streamable HTTP lands in the
// artist's undo history as one step with its label, shows in the snapshot and the panels, reverts
// with the UI's Undo, is reported by `events`, waits for an open gesture, and a commit still queued
// when the project is replaced fails with project-replaced. Real CompositionSession, offscreen.
#include "window_fixture.hpp"

#include <QAction>
#include <QTimer>
#include <bloom/mcp/server_service.hpp>
#include <bloom/mcp/testing/server_service_driver.hpp>
#include <bloom/platform/owner_only_secret.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/live_session_adapter.hpp>

#include <future>
#include <iostream>
#include <regex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace bloom;

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Predicate> bool waitFor(Predicate predicate, const int milliseconds = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return predicate();
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(const Socket socket) { ::closesocket(socket); }
using Length = int;
struct Startup final {
    Startup() {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~Startup() { ::WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(const Socket socket) { ::close(socket); }
using Length = std::size_t;
struct Startup final {};
#endif

struct Response final {
    int status = 0;
    std::string sessionId;
    std::string body;
};

// One blocking HTTP/1.1 exchange on its own connection (runs off the UI thread).
Response post(const std::uint16_t port, const std::string& token, const std::string& session,
              const std::string& body) {
    Response response;
    const Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == kInvalid)
        return response;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        closeSocket(socket);
        return response;
    }
    std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nAuthorization: Bearer " + token +
                          "\r\nContent-Type: application/json\r\nAccept: application/json, "
                          "text/event-stream\r\nConnection: close\r\n";
    if (!session.empty())
        request += "Mcp-Session-Id: " + session + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    static_cast<void>(::send(socket, request.data(), static_cast<Length>(request.size()), 0));
    std::string answer;
    char buffer[4096];
    while (true) {
        const auto got = ::recv(socket, buffer, static_cast<Length>(sizeof buffer), 0);
        if (got <= 0)
            break;
        answer.append(buffer, static_cast<std::size_t>(got));
        const auto head = answer.find("\r\n\r\n");
        if (head != std::string::npos) {
            std::smatch length;
            const std::string headers = answer.substr(0, head);
            if (std::regex_search(headers, length,
                                  std::regex("Content-Length: ([0-9]+)", std::regex::icase)) &&
                answer.size() >= head + 4 + std::stoul(length[1].str()))
                break;
        }
    }
    closeSocket(socket);
    if (answer.size() > 12)
        response.status = std::stoi(answer.substr(9, 3));
    std::smatch id;
    if (std::regex_search(answer, id, std::regex("Mcp-Session-Id: ([0-9a-f]+)")))
        response.sessionId = id[1].str();
    if (const auto head = answer.find("\r\n\r\n"); head != std::string::npos)
        response.body = answer.substr(head + 4);
    return response;
}

std::string call(const int id, const std::string& tool, const std::string& arguments) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + arguments +
           "}}";
}

void exercise() {
    [[maybe_unused]] const Startup startup;
    ui::test::WindowFixture fixture([](auto& fixture) {
        require(fixture.session.addSolidLayer("Background", {0.1, 0.5, 0.8, 1}), "Solid layer");
        QObject::connect(
            &fixture.projectHost, &ui::ProjectHost::sessionReplaced, &fixture.session, [&fixture] {
                const auto [document, stack] = fixture.projectHost.liveDocumentAndStack();
                if (document && stack)
                    fixture.session.rebind(*document, *stack,
                                           fixture.projectHost.lowestCompositionId());
            });
    });
    scripting::LiveSessionHost live;
    ui::LiveSessionAdapter adapter(live, fixture.session, fixture.projectHost);
    runtime::TaskScheduler scheduler;
    QTemporaryDir folder;
    require(folder.isValid(), "Scratch folder");
    const auto port = mcp::testing::McpServerServiceDriver::freePort(46000);
    mcp::McpServerService service({.live = live,
                                   .exportQueue = {},
                                   .scheduler = scheduler,
                                   .tokenFile = folder.filePath("mcp/token").toStdString(),
                                   .captureDirectory = folder.filePath("captures").toStdString(),
                                   .serverVersion = "test",
                                   .preferences = {.port = port, .autoStart = false},
                                   .limits = {}});
    // Test-only owner pump (the application-lifetime McpServerAdapter does this in the app).
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, [&service] { service.pump(); });
    pump.start(5);
    require(waitFor([&] { return service.snapshot()->token.state == mcp::McpTokenState::Ready; }),
            "Token ready");
    require(service.start().accepted, "Server started");
    const auto token = service.revealToken().token;

    const auto async = [&](auto work) {
        auto future = std::async(std::launch::async, std::move(work));
        require(waitFor([&] {
                    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                }),
                "HTTP exchange finished");
        return future.get();
    };
    const auto init = async([&] {
        return post(
            port, token, {},
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"undo-test","version":"1"}}})");
    });
    require(init.status == 200 && !init.sessionId.empty(), "Initialized over HTTP");
    const auto session = init.sessionId;
    require(async([&] {
                return post(port, token, session,
                            R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
            }).status == 202,
            "Initialized notification");

    // One transact = one undo step with its label, applied on the UI thread.
    const auto revision = fixture.session.snapshot().revision().value();
    const auto transact = async([&] {
        return post(
            port, token, session,
            call(
                2, "transact",
                R"({"expectedRevision":)" + std::to_string(revision) +
                    R"(,"label":"Agent rename","operations":[{"op":"bloom.project.set-name","args":{"name":"From agent"}},{"op":"bloom.composition.set-name","args":{"name":"Agent comp"}}]})"));
    });
    require(transact.status == 200 && transact.body.find("\"isError\":false") != std::string::npos,
            "Transact succeeded over HTTP");
    require(fixture.session.snapshot().project().name() == "From agent" &&
                fixture.session.snapshot().revision().value() == revision + 1,
            "The snapshot shows the agent's edit as one revision");
    auto* undo = fixture.window->findChild<QAction*>("undoAction");
    require(undo && undo->isEnabled() && undo->text().contains("Agent rename"),
            "The UI offers the agent's edit as one undo step with its label");
    undo->trigger();
    require(fixture.session.snapshot().project().name() != "From agent" &&
                fixture.session.composition()->name() != "Agent comp",
            "UI Undo reverts both operations");
    const auto events = async([&] {
        return post(port, token, session,
                    call(3, "events", R"({"sinceRevision":)" + std::to_string(revision) + "}"));
    });
    require(events.body.find("\\\"kind\\\":\\\"revision\\\"") != std::string::npos &&
                events.body.find("\\\"kind\\\":\\\"history\\\"") != std::string::npos,
            "events reports the commit and the UI undo");

    // A commit during a drag waits for the drag to end.
    const auto* position = fixture.session.parameterForSelection(document::kPositionParameterRole);
    require(position != nullptr, "Position parameter");
    require(fixture.session.beginValueEdit(position->id), "Drag begins");
    const auto during = fixture.session.snapshot().revision().value();
    auto waiting = std::async(std::launch::async, [&] {
        return post(
            port, token, session,
            call(
                4, "transact",
                R"({"expectedRevision":)" + std::to_string(during) +
                    R"(,"operations":[{"op":"bloom.project.set-name","args":{"name":"After drag"}}]})"));
    });
    require(waitFor([&] { return live.waitingForGesture(); }), "The commit waits for the gesture");
    QTest::qWait(150);
    require(fixture.session.snapshot().project().name() != "After drag",
            "Nothing applies while the drag is open");
    fixture.session.cancelValueEdit();
    require(waitFor([&] {
                return waiting.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
            "The commit finishes after the drag");
    require(waiting.get().body.find("\"isError\":false") != std::string::npos &&
                fixture.session.snapshot().project().name() == "After drag",
            "The commit applies after the drag ends");
    require(undo->text().contains("Agent edit"), "An unlabelled commit is labelled Agent edit");

    // A commit still queued when the project is replaced fails with project-replaced.
    require(fixture.session.beginValueEdit(position->id), "Hold a gesture");
    const auto held = fixture.session.snapshot().revision().value();
    auto replaced = std::async(std::launch::async, [&] {
        return post(
            port, token, session,
            call(5, "transact",
                 R"({"expectedRevision":)" + std::to_string(held) +
                     R"(,"operations":[{"op":"bloom.project.set-name","args":{"name":"Lost"}}]})"));
    });
    require(waitFor([&] { return live.queuedCommits() == 1; }), "The commit is queued");
    fixture.session.cancelValueEdit();
    fixture.projectHost.setUnsavedChangeDecisionProvider(
        [] { return ui::UnsavedChangeDecision::Discard; });
    // Replace before the drain slice runs.
    fixture.projectHost.newProject();
    require(waitFor([&] {
                return replaced.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
            "The queued commit ends");
    const auto lost = replaced.get();
    require(lost.body.find("bloom.mcp.project-replaced") != std::string::npos,
            "A commit queued across New fails with project-replaced");
    require(fixture.session.snapshot().project().name() != "Lost",
            "Nothing reached the new project");

    service.beginShutdown();
    require(waitFor([&] { return service.quiescent(); }), "Server shut down");
    pump.stop();
    adapter.shutdown();
    scheduler.beginShutdown();
    std::cout << "MCP live undo: transact over HTTP, label, UI undo, events, gesture wait and "
                 "replacement passed\n";
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomTests");
    app.setApplicationName("McpLiveUndo");
    ui::kit::installKinetikTheme(app);
    try {
        exercise();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
