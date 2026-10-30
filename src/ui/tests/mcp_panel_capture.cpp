// Captures of the MCP Server panel over a scripted port: stopped, running with two clients and
// activity, a port-in-use failure, and a narrow panel. Usage: bloom_mcp_panel_capture [directory].
// Writes <directory>/<state>-dpr1.png or -dpr15.png by QT_SCALE_FACTOR.
#include "mcp_test_support.hpp"

#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/mcp_server_editor.hpp>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QTest>

#include <iostream>

namespace {

namespace ui = bloom::ui;
namespace m = bloom::mcp;
using ui::mcptest::ScriptedMcpPort;

int capture(ScriptedMcpPort& port, ui::McpServerAdapter& adapter, const QString& directory,
            const QString& scale, const QString& name, const QSize size) {
    (void)port;
    ui::McpServerEditor panel(&adapter);
    panel.resize(size);
    panel.show();
    QTest::qWait(60);
    const QImage image = panel.grab().toImage();
    const QString path = directory + QStringLiteral("/%1-dpr%2.png").arg(name, scale);
    if (!image.save(path)) {
        std::cerr << "cannot write " << path.toStdString() << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomMcpPanelCapture");
    app.setApplicationName("McpPanelCapture");
    ui::kit::installKinetikTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).remove('.');
    int failures = 0;
    {
        ScriptedMcpPort port;
        ui::McpServerAdapter adapter(port);
        failures += capture(port, adapter, directory, scale, QStringLiteral("stopped"), {760, 820});
    }
    {
        ScriptedMcpPort port;
        ui::McpServerAdapter adapter(port);
        auto running = ui::mcptest::makeSnapshot(m::McpServerState::Running);
        running.clients = {ui::mcptest::makeClient(1, "claude-code", "2.1.0"),
                           ui::mcptest::makeClient(2, "vscode-mcp", "1.9.2")};
        running.clients[0].lastTool = m::McpTool::Transact;
        const m::McpTool tools[] = {m::McpTool::Query, m::McpTool::Transact, m::McpTool::Capture,
                                    m::McpTool::ExportStart, m::McpTool::Events};
        port.addEvent(m::McpClientConnected{running.clients[0]});
        for (std::uint64_t id = 1; id <= 9; ++id) {
            auto entry = ui::mcptest::makeToolCall(id, id % 2 ? "claude-code" : "vscode-mcp", tools[id % 5]);
            if (id == 6) {
                entry.outcome = m::McpCallOutcome::ToolError;
                entry.toolError = m::McpToolErrorCode::RevisionConflict;
            }
            if (id == 8) {
                entry.kind = m::McpActivityKind::RequestRejected;
                entry.rejection = m::McpHttpRejection::Unauthorized;
                entry.repeat = 3;
            }
            port.addActivity(entry);
        }
        port.stage(running);
        adapter.drain();
        failures += capture(port, adapter, directory, scale, QStringLiteral("running"), {760, 820});
    }
    {
        ScriptedMcpPort port;
        ui::McpServerAdapter adapter(port);
        auto failed = ui::mcptest::makeSnapshot(m::McpServerState::Stopped);
        failed.failure = m::McpServerFailure{
            m::McpServerFailureCode::PortInUse, 6767,
            "Port 6767 is in use. Choose another port or stop the other program.", {}};
        port.stage(failed);
        adapter.drain();
        failures += capture(port, adapter, directory, scale, QStringLiteral("port-in-use"), {760, 820});
    }
    {
        ScriptedMcpPort port;
        ui::McpServerAdapter adapter(port);
        auto running = ui::mcptest::makeSnapshot(m::McpServerState::Running);
        running.clients = {ui::mcptest::makeClient(1, "claude-code", "2.1.0")};
        port.addActivity(ui::mcptest::makeToolCall(1, "claude-code", m::McpTool::Query));
        port.stage(running);
        adapter.drain();
        failures += capture(port, adapter, directory, scale, QStringLiteral("narrow"), {340, 820});
    }
    return failures;
}
