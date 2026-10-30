#include <bloom/ui/mcp_server_text.hpp>

#include <QCoreApplication>
#include <QDateTime>

namespace bloom::ui::mcppanel {
namespace {
QString tr(const char* text) { return QCoreApplication::translate("McpServerPanel", text); }

QString rejectionText(const mcp::McpHttpRejection rejection) {
    using R = mcp::McpHttpRejection;
    switch (rejection) {
    case R::HostNotAllowed:
        return tr("host not allowed");
    case R::OriginNotAllowed:
        return tr("origin not allowed");
    case R::NotFound:
        return tr("wrong address");
    case R::Unauthorized:
        return tr("wrong or missing token");
    case R::MethodNotAllowed:
        return tr("method not allowed");
    case R::UnsupportedMediaType:
    case R::NotAcceptable:
    case R::UnsupportedProtocolVersion:
    case R::BatchNotSupported:
        return tr("unsupported request");
    case R::MissingSession:
    case R::UnknownSession:
        return tr("no session");
    case R::PayloadTooLarge:
        return tr("request too large");
    case R::Busy:
        return tr("server busy");
    case R::Stopping:
        return tr("server stopping");
    case R::None:
        break;
    }
    return {};
}

QString toolErrorText(const mcp::McpToolErrorCode code) {
    using C = mcp::McpToolErrorCode;
    switch (code) {
    case C::ProjectUnavailable:
        return tr("no project");
    case C::ProjectReplaced:
        return tr("project replaced");
    case C::RevisionConflict:
        return tr("revision conflict");
    case C::CommitQueueFull:
        return tr("queue full");
    case C::GestureOpen:
        return tr("edit in progress");
    case C::Cancelled:
        return tr("cancelled");
    case C::DeadlineExceeded:
        return tr("timed out");
    case C::ExportUnavailable:
        return tr("export unavailable");
    case C::ExportFailed:
        return tr("export failed");
    case C::CaptureTooLargeForInline:
        return tr("capture too large");
    case C::ReadOnly:
        return tr("project is read-only");
    case C::OperationRejected:
        return tr("operation rejected");
    case C::PreflightBlocked:
        return tr("cannot render as asked");
    }
    return {};
}

// A successful project call as a plain line: "saved 'SC020'". The service records the project's
// name after the tool name in the entry's summary ("project.save SC020").
QString projectLine(const mcp::McpActivityEntry& entry) {
    using T = mcp::McpTool;
    if (!entry.tool || !entry.outcome || *entry.outcome != mcp::McpCallOutcome::Succeeded)
        return {};
    const auto toolName = std::string(mcp::toolName(*entry.tool));
    if (entry.summary.size() <= toolName.size() + 1 || !entry.summary.starts_with(toolName + " "))
        return {};
    const auto name = qs(entry.summary.substr(toolName.size() + 1));
    switch (*entry.tool) {
    case T::ProjectSave:
    case T::ProjectSaveAs:
        return tr("saved '%1'").arg(name);
    case T::ProjectOpen:
        return tr("opened '%1'").arg(name);
    case T::ProjectNew:
        return tr("created '%1'").arg(name);
    case T::ProjectClose:
        return tr("closed '%1'").arg(name);
    default:
        return {};
    }
}
} // namespace

QString statusText(const mcp::McpServerSnapshot& snapshot) {
    using S = mcp::McpServerState;
    // A refused start leaves the server Stopped with the failure kept: it stays on the status line.
    if (snapshot.failure.has_value() && snapshot.state != S::Running &&
        snapshot.state != S::Stopping)
        // A failure about the port is spelled out under the Port field; the status stays short.
        return snapshot.failure->port != 0
                   ? QCoreApplication::translate("McpServerPanel", "Couldn't start")
                   : QCoreApplication::translate("McpServerPanel", "Couldn't start: %1")
                         .arg(qs(snapshot.failure->reason));
    switch (snapshot.state) {
    case S::Stopped:
        return tr("Stopped");
    case S::Stopping:
        return tr("Stopping…");
    case S::Failed:
        return tr("Stopped");
    case S::Running: {
        const auto host = snapshot.endpoint ? qs(snapshot.endpoint->host)
                                            : QStringLiteral("127.0.0.1");
        const auto port = snapshot.endpoint ? snapshot.endpoint->port : snapshot.preferences.port;
        const auto clients = static_cast<int>(snapshot.clients.size());
        const auto count = clients == 1 ? tr("1 client")
                                        : tr("%1 clients").arg(clients);
        return tr("Running on %1:%2 · %3").arg(host).arg(port).arg(count);
    }
    }
    return {};
}

QString timeText(const std::chrono::system_clock::time_point at) {
    if (at == std::chrono::system_clock::time_point{})
        return {};
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch());
    return QDateTime::fromSecsSinceEpoch(seconds.count()).toString(QStringLiteral("HH:mm:ss"));
}

QString clientName(const mcp::McpClientInfo& info) {
    return qs(info.title.empty() ? info.name : info.title);
}

QString toolText(const std::optional<mcp::McpTool>& tool) {
    return tool.has_value() ? QString::fromUtf8(mcp::toolName(*tool).data(),
                                                static_cast<qsizetype>(mcp::toolName(*tool).size()))
                            : QString{};
}

QString activityLine(const mcp::McpActivityEntry& entry) {
    using K = mcp::McpActivityKind;
    const auto label = qs(entry.clientLabel);
    QString what;
    switch (entry.kind) {
    case K::ServerStarted:
        what = tr("Server started");
        break;
    case K::ServerStopped:
        what = tr("Server stopped");
        break;
    case K::StartRefused:
        what = tr("Start refused");
        break;
    case K::ListenerFailed:
        what = tr("Server stopped unexpectedly");
        break;
    case K::ClientConnected:
        what = tr("Connected");
        break;
    case K::ClientDisconnected:
        what = tr("Disconnected");
        break;
    case K::ProjectReplaced:
        what = tr("Project replaced");
        break;
    case K::TokenRegenerated:
        what = tr("Token regenerated");
        break;
    case K::TokenRevealed:
        what = tr("Token revealed");
        break;
    case K::RequestRejected:
        what = tr("Request refused");
        if (entry.rejection.has_value())
            if (const auto reason = rejectionText(*entry.rejection); !reason.isEmpty())
                what += QStringLiteral(": ") + reason;
        break;
    case K::ToolCall: {
        if (const auto project = projectLine(entry); !project.isEmpty()) {
            what = project;
            break;
        }
        what = toolText(entry.tool);
        if (what.isEmpty() && !entry.summary.empty())
            what = qs(entry.summary);
        using O = mcp::McpCallOutcome;
        if (entry.outcome.has_value()) {
            QString outcome;
            switch (*entry.outcome) {
            case O::Succeeded:
                outcome = tr("done");
                break;
            case O::ToolError:
                outcome = tr("failed");
                if (entry.toolError.has_value())
                    if (const auto reason = toolErrorText(*entry.toolError); !reason.isEmpty())
                        outcome += QStringLiteral(": ") + reason;
                break;
            case O::ProtocolError:
                outcome = tr("refused");
                break;
            case O::Cancelled:
                outcome = tr("cancelled");
                break;
            }
            what += QStringLiteral(" · ") + outcome;
        }
        if (entry.duration.count() > 0)
            what += QStringLiteral(" · ") + tr("%1 ms").arg(entry.duration.count());
        break;
    }
    }
    if (what.isEmpty())
        what = qs(entry.summary);
    if (entry.repeat > 1)
        what += QStringLiteral(" ×") + QString::number(entry.repeat);
    QString line = timeText(entry.at);
    if (!label.isEmpty())
        line += (line.isEmpty() ? QString{} : QStringLiteral("  ")) + label;
    line += (line.isEmpty() ? QString{} : QStringLiteral("  ")) + what;
    return line;
}

QString compileFailureLine(const McpCompileFailureNote& note) {
    QStringList parts;
    if (!note.client.isEmpty())
        parts << note.client;
    parts << (note.tool.isEmpty() ? QStringLiteral("transact") : note.tool);
    QString what = tr("left '%1' unable to render").arg(note.composition);
    if (!note.detail.isEmpty())
        what += QStringLiteral(": ") + note.detail;
    parts << what;
    const auto time = timeText(note.at);
    return (time.isEmpty() ? QString{} : time + QStringLiteral("  ")) +
           parts.join(QStringLiteral(" · "));
}

QString clientKindText(const mcp::McpClientKind kind) {
    switch (kind) {
    case mcp::McpClientKind::ClaudeCode:
        return tr("Claude Code");
    case mcp::McpClientKind::McpJson:
        return tr("MCP JSON");
    case mcp::McpClientKind::VsCode:
        return tr("VS Code");
    case mcp::McpClientKind::Generic:
        return tr("Other");
    }
    return {};
}

} // namespace bloom::ui::mcppanel
