// The MCP Server panel (ADR 0034, section 7) over a scripted port: adapter drains, status text,
// availability reasons, the port field's inline reasons, token masking, regeneration confirmation,
// clients, incremental activity, the copy-config menu, the status bar indicator and the warning
// sound.
#include "mcp_test_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/mcp_server_editor.hpp>
#include <bloom/ui/mcp_server_text.hpp>
#include <bloom/ui/mcp_server_service_port.hpp>
#include <bloom/mcp/testing/server_service_driver.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <filesystem>

#include <bloom/ui/ui_feedback.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <iostream>
#include <source_location>
#include <string_view>

namespace {

namespace ui = bloom::ui;
namespace m = bloom::mcp;
using ui::mcptest::ScriptedMcpPort;
using ui::mcptest::makeSnapshot;

int gFailures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location where = std::source_location::current()) {
    if (condition)
        return;
    ++gFailures;
    std::cerr << where.file_name() << ':' << where.line() << ": " << message << '\n';
}

template <typename T> T* find(QWidget& root, const char* name) {
    return root.findChild<T*>(QString::fromLatin1(name));
}

// Publishes `snapshot` the way the service does: staged, then pumped by the adapter's drain.
void show(ScriptedMcpPort& port, ui::McpServerAdapter& adapter, m::McpServerSnapshot snapshot) {
    port.stage(std::move(snapshot));
    adapter.drain();
}

void testAdapterDrain() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    QSignalSpy changed(&adapter, &ui::McpServerAdapter::changed);
    adapter.drain();
    expect(changed.count() == 0 && port.pumps == 1, "a drain with no new revision pumps and is silent");
    port.stage(makeSnapshot(m::McpServerState::Running));
    adapter.drain();
    adapter.drain();
    expect(changed.count() == 1, "one revision gives exactly one changed()");
    expect(adapter.snapshot()->state == m::McpServerState::Running, "the adapter reads the pumped snapshot");

    // The waiter coalesces several wakes into the drains that run on the main thread.
    adapter.startWaiter();
    port.stage(makeSnapshot(m::McpServerState::Stopped));
    port.publish();
    port.publish();
    port.publish();
    // The waiter thread may start after the first publish; keep waking until it has drained.
    for (int i = 0; i < 100 && changed.count() < 2; ++i) {
        port.publish();
        QTest::qWait(10);
    }
    expect(changed.count() == 2, "the waiter's wakes drain once and emit one changed()");
}

void testPreferencesPersist() {
    QSettings settings;
    settings.remove(QStringLiteral("mcp"));
    expect(ui::loadMcpServerPreferences(settings) == m::McpServerPreferences{}, "defaults: 6767, auto-start off");
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    auto changed = makeSnapshot(m::McpServerState::Stopped, 7000);
    changed.preferences.autoStart = true;
    changed.preferencesRevision = 1;
    show(port, adapter, changed);
    const auto loaded = ui::loadMcpServerPreferences(QSettings());
    expect(loaded.port == 7000 && loaded.autoStart, "the adapter persists a moved preferences revision");
    settings.setValue(QStringLiteral("mcp/port"), 80);
    expect(ui::loadMcpServerPreferences(settings).port == 6767, "a stored port out of range falls back");
    settings.remove(QStringLiteral("mcp"));
}

void testStatusText() {
    using S = m::McpServerState;
    auto stopped = makeSnapshot(S::Stopped);
    expect(ui::mcppanel::statusText(stopped) == QStringLiteral("Stopped"), "stopped text");
    auto running = makeSnapshot(S::Running);
    running.clients = {ui::mcptest::makeClient(1, "claude", "1"), ui::mcptest::makeClient(2, "x", "2")};
    expect(ui::mcppanel::statusText(running) == QStringLiteral("Running on 127.0.0.1:6767 · 2 clients"),
           "running text with clients");
    running.clients.resize(1);
    expect(ui::mcppanel::statusText(running).endsWith(QStringLiteral("1 client")), "singular client");
    expect(ui::mcppanel::statusText(makeSnapshot(S::Stopping)) == QStringLiteral("Stopping…"),
           "stopping text");
    auto failed = makeSnapshot(S::Stopped);
    failed.failure = m::McpServerFailure{m::McpServerFailureCode::PortInUse, 6767,
                                         "Port 6767 is in use. Choose another port or stop the other program.",
                                         {}};
    expect(ui::mcppanel::statusText(failed) ==
               QStringLiteral("Couldn't start"),
           "a port failure keeps the status short; the reason sits under the Port field");
}

using Refusal = m::McpUnavailableCode;

QString reasonOf(const m::McpCommandAvailability& a) {
    return ui::mcppanel::qs(a.reason);
}

// Everything the panel shows as text, for the "the token never leaks" checks.
QStringList allText(QWidget& panel) {
    QStringList texts;
    for (auto* label : panel.findChildren<QLabel*>())
        texts << label->text() << label->toolTip();
    for (auto* button : panel.findChildren<QAbstractButton*>())
        texts << button->text() << button->toolTip();
    for (auto* list : panel.findChildren<QListWidget*>())
        for (int row = 0; row < list->count(); ++row)
            texts << list->item(row)->text() << list->item(row)->toolTip();
    return texts;
}

bool anyContains(const QStringList& texts, const QString& needle) {
    for (const auto& text : texts)
        if (text.contains(needle))
            return true;
    return false;
}

void testAvailability() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    ui::McpServerEditor panel(&adapter);
    panel.resize(720, 800);
    panel.show();
    auto* button = find<ui::kit::KButton>(panel, "mcpStartStopButton");
    auto* startReason = find<ui::kit::KLabel>(panel, "mcpStartReason");
    expect(button && button->text() == QStringLiteral("Start") && button->isEnabled(),
           "stopped: Start is available");
    expect(find<ui::kit::KLabel>(panel, "mcpStatus")->text() == QStringLiteral("Stopped"), "status line");
    button->click();
    expect(port.starts == 1, "Start issues the command");

    // Start refused while the token loads: disabled, with the reason as tooltip AND inline.
    auto loading = makeSnapshot(m::McpServerState::Stopped);
    loading.token.state = m::McpTokenState::Loading;
    auto& start = loading.commands.entries[static_cast<size_t>(m::McpServerCommand::Start)];
    start = ui::mcptest::refused(Refusal::TokenLoading, m::McpField::Status,
                                 "Bloom is still loading the access token.");
    for (auto command : {m::McpServerCommand::RevealToken, m::McpServerCommand::RegenerateToken})
        loading.commands.entries[static_cast<size_t>(command)] = ui::mcptest::refused(
            Refusal::TokenLoading, m::McpField::Token, "Bloom is still loading the access token.");
    show(port, adapter, loading);
    expect(!button->isEnabled() && button->toolTip() == reasonOf(start), "Start tooltip carries the reason");
    expect(startReason->isVisibleTo(&panel) && startReason->text() == reasonOf(start),
           "Start reason is shown inline");
    for (const char* name : {"mcpTokenRevealButton", "mcpTokenCopyButton", "mcpTokenRegenerateButton"}) {
        auto* control = find<ui::kit::KButton>(panel, name);
        expect(control && !control->isEnabled() &&
                   control->toolTip() == QStringLiteral("Bloom is still loading the access token."),
               "a token control has its reason as tooltip");
    }
    expect(find<ui::kit::KLabel>(panel, "mcpTokenReason")->isVisibleTo(&panel), "token reason inline");

    // Running: the one button becomes Stop; the port and its reason follow ServerRunning.
    show(port, adapter, makeSnapshot(m::McpServerState::Running));
    expect(button->text() == QStringLiteral("Stop") && button->isEnabled(), "running: Stop");
    button->click();
    expect(port.stops == 1, "Stop issues the command");
    auto* portField = find<ui::kit::KValueField>(panel, "mcpPortField");
    auto* portReason = find<ui::kit::KLabel>(panel, "mcpPortReason");
    expect(!portField->isEnabled() && portField->toolTip() == QStringLiteral("Stop the server to change the port."),
           "the port field is locked with its reason as tooltip");
    expect(portReason->isVisibleTo(&panel) &&
               portReason->text() == QStringLiteral("Stop the server to change the port."),
           "the port reason is inline");

    // Stopping: Stop is shown disabled with its reason.
    show(port, adapter, makeSnapshot(m::McpServerState::Stopping));
    expect(!button->isEnabled() && button->toolTip() == QStringLiteral("The server is stopping."),
           "stopping: Stop disabled with reason");
    expect(find<ui::kit::KLabel>(panel, "mcpStatus")->text() == QStringLiteral("Stopping…"), "stopping status");
}

void testPortField() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    ui::McpServerEditor panel(&adapter);
    panel.show();
    auto* field = find<ui::kit::KValueField>(panel, "mcpPortField");
    auto* reason = find<ui::kit::KLabel>(panel, "mcpPortReason");
    expect(field->integerValue() == 6767, "default port 6767");
    field->setIntegerValue(80);
    expect(reason->isVisibleTo(&panel) &&
               reason->text() == QStringLiteral("Port 80 can't be used. Choose a port from 1024 to 65535."),
           "an out-of-range port shows validateMcpPort's reason");
    expect(port.setPortCalls.empty(), "an out-of-range port is not sent");
    field->setIntegerValue(7000);
    expect(port.setPortCalls == std::vector<std::uint32_t>{7000} && !reason->isVisibleTo(&panel),
           "a valid port is set and the reason clears");

    // A setPort refusal from the service appears under the field.
    m::McpCommandResult refusal;
    refusal.availability = ui::mcptest::refused(Refusal::ServerRunning, m::McpField::Port,
                                                "Stop the server to change the port.");
    port.setPortResult = refusal;
    field->setIntegerValue(7100);
    expect(reason->isVisibleTo(&panel) && reason->text() == QStringLiteral("Stop the server to change the port."),
           "a setPort refusal is shown inline");

    // A start refused for a taken port stays under the field and on the status line.
    port.setPortResult.reset();
    field->setIntegerValue(6767);
    auto failed = makeSnapshot(m::McpServerState::Stopped);
    failed.failure = m::McpServerFailure{m::McpServerFailureCode::PortInUse, 6767,
                                         "Port 6767 is in use. Choose another port or stop the other program.", {}};
    show(port, adapter, failed);
    expect(reason->text().startsWith(QStringLiteral("Port 6767 is in use.")), "start refusal under the port");
    expect(find<ui::kit::KLabel>(panel, "mcpStatus")->text() == QStringLiteral("Couldn't start"),
           "start refusal on the status line");

    auto* autoStart = find<ui::kit::KSwitch>(panel, "mcpAutoStartSwitch");
    autoStart->click();
    expect(port.autoStartCalls == std::vector<bool>{true}, "auto-start switch issues setAutoStart");
}

void testToken() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    ui::McpServerEditor panel(&adapter);
    panel.resize(720, 800);
    panel.show();
    const auto secret = QString::fromStdString(port.token);
    auto* token = find<ui::kit::KLabel>(panel, "mcpToken");
    auto* revealButton = find<ui::kit::KButton>(panel, "mcpTokenRevealButton");
    auto snapshot = makeSnapshot(m::McpServerState::Running);
    snapshot.clients = {ui::mcptest::makeClient(1, "claude-code", "2.1")};
    show(port, adapter, snapshot);
    port.addActivity(ui::mcptest::makeToolCall(1, "claude-code", m::McpTool::Query));
    auto withActivity = snapshot;
    show(port, adapter, withActivity);
    expect(token->text() == QStringLiteral("blm_ab12••••••••9xyz"), "the token is masked at rest");
    expect(!anyContains(allText(panel), secret) && port.reveals == 0, "the token is not on screen at rest");

    revealButton->click();
    expect(token->text() == secret && port.reveals == 1, "Reveal shows the token");
    // Focus leaving the panel hides it again.
    Q_EMIT qApp->focusChanged(revealButton, nullptr);
    expect(token->text() == QStringLiteral("blm_ab12••••••••9xyz"), "losing focus re-masks the token");
    revealButton->click();
    expect(token->text() == secret, "revealed again");
    auto* timer = panel.findChild<QTimer*>(QStringLiteral("mcpRevealTimer"));
    expect(timer && timer->isActive() && timer->interval() == 30000, "a 30 s timer is armed");
    QMetaObject::invokeMethod(timer, "timeout");
    expect(token->text() == QStringLiteral("blm_ab12••••••••9xyz"), "the timer re-masks the token");
    expect(!anyContains(allText(panel), secret), "no other text carries the token");

    // Copy puts it on the clipboard without showing it.
    find<ui::kit::KButton>(panel, "mcpTokenCopyButton")->click();
    expect(QGuiApplication::clipboard()->text() == secret, "Copy puts the token on the clipboard");
    expect(token->text() != secret, "Copy does not reveal");

    // Regenerating shows its own state.
    auto regenerating = snapshot;
    regenerating.token.state = m::McpTokenState::Regenerating;
    show(port, adapter, regenerating);
    expect(token->text() == QStringLiteral("Regenerating…"), "Regenerating… state");
}

void testRegenerateConfirms() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    ui::McpServerEditor panel(&adapter);
    panel.show();
    auto* regenerate = find<ui::kit::KButton>(panel, "mcpTokenRegenerateButton");
    regenerate->click();
    auto* dialog = panel.window()->findChild<ui::kit::KDialog*>(QStringLiteral("mcpRegenerateConfirm"));
    expect(dialog != nullptr && port.regenerates == 0, "Regenerate asks first");
    if (dialog == nullptr)
        return;
    expect(dialog->findChildren<QLabel*>().size() > 0, "the confirmation has text");
    bool mentionsClients = false;
    for (auto* label : dialog->findChildren<QLabel*>())
        mentionsClients = mentionsClients || label->text().contains(QStringLiteral("connected client"));
    expect(mentionsClients, "it says every connected client will be disconnected");
    (void)dialog->activate(ui::kit::KDialog::Role::Reject);
    QTest::qWait(20);
    expect(port.regenerates == 0, "Cancel does not regenerate");
    regenerate->click();
    dialog = panel.window()->findChild<ui::kit::KDialog*>(QStringLiteral("mcpRegenerateConfirm"));
    if (dialog != nullptr)
        (void)dialog->activate(ui::kit::KDialog::Role::Destructive);
    QTest::qWait(20);
    expect(port.regenerates == 1, "Confirming regenerates once");
}

void testClientsAndActivity() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    ui::McpServerEditor panel(&adapter);
    panel.resize(900, 900);
    panel.show();
    auto snapshot = makeSnapshot(m::McpServerState::Running);
    snapshot.clients = {ui::mcptest::makeClient(1, "claude-code", "2.1.0"),
                        ui::mcptest::makeClient(2, "vscode", "1.9")};
    show(port, adapter, snapshot);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    int rows = 0;
    for (auto* row : panel.findChildren<ui::kit::KRow*>(QStringLiteral("mcpClientRow")))
        rows += row->isHidden() ? 0 : 1;
    expect(rows == 2, "two client rows, got " + std::to_string(rows));
    bool sawName = false;
    for (auto* label : panel.findChildren<ui::kit::KLabel*>(QStringLiteral("mcpClientName")))
        sawName = sawName || label->toolTip() == QStringLiteral("claude-code");
    expect(sawName, "client names render");
    expect(find<ui::kit::KLabel>(panel, "mcpStatus")->text().endsWith(QStringLiteral("2 clients")),
           "status counts clients");

    // Activity appends incrementally.
    auto* list = find<QListWidget>(panel, "mcpActivityList");
    port.addActivity(ui::mcptest::makeToolCall(1, "claude-code", m::McpTool::Query));
    port.addActivity(ui::mcptest::makeToolCall(2, "claude-code", m::McpTool::Transact));
    show(port, adapter, snapshot);
    expect(list->count() == 2, "two activity lines");
    expect(list->item(1)->text().contains(QStringLiteral("claude-code")) &&
               list->item(1)->text().contains(QStringLiteral("transact")) &&
               list->item(1)->text().contains(QStringLiteral("done")),
           "a line is built from the typed facts");
    port.addActivity(ui::mcptest::makeToolCall(3, "vscode", m::McpTool::Events));
    show(port, adapter, snapshot);
    expect(list->count() == 3 && port.activityCalls.back() == 2, "only the new entry is read and appended");

    // A trim shows the line once, at the top.
    for (std::uint64_t id = 4; id <= 7; ++id)
        port.addActivity(ui::mcptest::makeToolCall(id, "vscode", m::McpTool::Query));
    port.trimActivity(5);
    show(port, adapter, snapshot);
    expect(list->item(0)->text() == QStringLiteral("Earlier activity was trimmed."), "gap line");

    // A fallback summary is used only when nothing typed explains the entry.
    m::McpActivityEntry odd;
    odd.id = 8;
    odd.kind = m::McpActivityKind::ToolCall;
    odd.summary = "custom summary";
    port.addActivity(odd);
    show(port, adapter, snapshot);
    expect(list->item(list->count() - 1)->text().contains(QStringLiteral("custom summary")), "summary fallback");

    snapshot.project.bound = false;
    snapshot.project.reason = "A project is opening.";
    show(port, adapter, snapshot);
    auto* project = find<ui::kit::KLabel>(panel, "mcpProjectLine");
    expect(project->isVisibleTo(&panel) && project->text() == QStringLiteral("A project is opening."),
           "project line");
}

void testCopyConfig() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    ui::McpServerEditor panel(&adapter);
    panel.show();
    auto* button = find<ui::kit::KMenuButton>(panel, "mcpCopyConfigButton");
    int entries = 0;
    for (auto* action : button->menu()->actions())
        entries += action->isSeparator() ? 0 : 1;
    expect(entries == 8, "the copy menu has eight entries");
    auto* placeholder = button->menu()->findChild<QAction*>(QStringLiteral("mcpCopyConfig_vsCode_placeholder"));
    expect(placeholder && placeholder->text().contains(QStringLiteral("${BLOOM_MCP_TOKEN}")), "placeholder variant");
    if (placeholder != nullptr) {
        placeholder->trigger();
        expect(QGuiApplication::clipboard()->text().contains(QStringLiteral("${BLOOM_MCP_TOKEN}")),
               "the text is the host's, copied as is");
    }
    auto locked = makeSnapshot(m::McpServerState::Stopped);
    locked.commands.entries[static_cast<size_t>(m::McpServerCommand::CopyClientConfig)] =
        ui::mcptest::refused(Refusal::TokenLoading, m::McpField::Token, "Bloom is still loading the access token.");
    show(port, adapter, locked);
    auto* withToken = button->menu()->findChild<QAction*>(QStringLiteral("mcpCopyConfig_other_token"));
    expect(withToken && !withToken->isEnabled() && withToken->toolTip().startsWith(QStringLiteral("Bloom is still")),
           "token variants carry the reason when unavailable");
    expect(button->menu()->findChild<QAction*>(QStringLiteral("mcpCopyConfig_other_placeholder"))->isEnabled(),
           "the placeholder variants stay available");
}

void testCompileFailureActivity() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    show(port, adapter, makeSnapshot(m::McpServerState::Running));
    ui::McpServerEditor panel(&adapter);
    panel.resize(760, 820);
    panel.show();
    auto* list = find<QListWidget>(panel, "mcpActivityList");
    ui::McpCompileFailureNote note;
    note.client = QStringLiteral("Claude Code");
    note.tool = QStringLiteral("transact");
    note.composition = QStringLiteral("Main");
    note.detail = QStringLiteral("Blending Mode is driven by Scalar to Integer.");
    adapter.noteCompileFailure(note);
    expect(list != nullptr && list->count() == 1, "a compile failure adds one Activity line");
    if (list == nullptr || list->count() != 1)
        return;
    const auto line = list->item(0)->text();
    expect(line.contains(QStringLiteral("Claude Code \u00b7 transact \u00b7 left 'Main' unable to render: "
                                        "Blending Mode is driven by Scalar to Integer.")),
           "the line reads client, tool, composition and detail");
    expect(!line.contains(QStringLiteral("bloom.")), "no raw code in the line");
    expect(list->item(0)->foreground().color() == ui::kit::color(ui::kit::Color::Error),
           "the line is in the error tone");
    // A reopened panel shows the failures already noted.
    ui::McpServerEditor again(&adapter);
    again.show();
    auto* list2 = find<QListWidget>(again, "mcpActivityList");
    expect(list2 != nullptr && list2->count() == 1, "a new panel lists the earlier failure");
}

// A transact activity entry that carries compileFailures becomes one red Activity line, once.
void testCompileFailureFromActivity() {
    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    adapter.setCompositionNamer([](const std::uint64_t id) {
        return id == 7 ? QStringLiteral("Main") : QString{};
    });
    show(port, adapter, makeSnapshot(m::McpServerState::Running));
    ui::McpServerEditor panel(&adapter);
    panel.resize(760, 820);
    panel.show();
    auto* list = find<QListWidget>(panel, "mcpActivityList");

    m::McpActivityEntry entry;
    entry.id = 41;
    entry.kind = m::McpActivityKind::ToolCall;
    entry.clientLabel = "Claude Code";
    entry.tool = m::McpTool::Transact;
    entry.outcome = m::McpCallOutcome::Succeeded;
    entry.summary = "transact";
    entry.compileFailures.push_back({.composition = 7,
                                     .detail = "Blending Mode is driven by Scalar to Integer.",
                                     .node = 3,
                                     .parameter = 9});
    entry.compileFailures.push_back({.composition = 8, .detail = "Layer Output could not be lowered."});
    port.addActivity(entry);
    adapter.drain();
    expect(adapter.compileFailures().size() == 2, "each failed composition is noted once");
    adapter.drain();
    expect(adapter.compileFailures().size() == 2, "a second drain does not repeat the notes");
    if (adapter.compileFailures().size() != 2)
        return;
    const auto& first = adapter.compileFailures()[0];
    expect(first.client == QStringLiteral("Claude Code") && first.tool == QStringLiteral("transact") &&
               first.composition == QStringLiteral("Main") &&
               first.detail == QStringLiteral("Blending Mode is driven by Scalar to Integer."),
           "the note names the client, tool, composition and plain detail");
    expect(adapter.compileFailures()[1].composition == QStringLiteral("Composition 8"),
           "a composition without a name is numbered");
    expect(list != nullptr && list->count() >= 2, "the panel lists the failures");
    bool found = false;
    for (int row = 0; list != nullptr && row < list->count(); ++row)
        if (list->item(row)->text().contains(QStringLiteral("left 'Main' unable to render")) &&
            list->item(row)->foreground().color() == ui::kit::color(ui::kit::Color::Error))
            found = true;
    expect(found, "the MCP compile failure is a red Activity line");
}

// Project calls read as plain sentences in the Activity list.
void testProjectActivityLines() {
    const auto line = [](const m::McpTool tool, const std::string& summary) {
        m::McpActivityEntry entry;
        entry.id = 1;
        entry.kind = m::McpActivityKind::ToolCall;
        entry.clientLabel = "claude-code";
        entry.tool = tool;
        entry.outcome = m::McpCallOutcome::Succeeded;
        entry.summary = std::string(m::toolName(tool)) + " " + summary;
        return ui::mcppanel::activityLine(entry);
    };
    expect(line(m::McpTool::ProjectSave, "SC020").endsWith(QStringLiteral("claude-code  saved 'SC020'")),
           "project.save reads \"saved 'SC020'\"");
    expect(line(m::McpTool::ProjectSaveAs, "SC020").contains(QStringLiteral("saved 'SC020'")),
           "project.save-as reads as saved");
    expect(line(m::McpTool::ProjectOpen, "My Reel").contains(QStringLiteral("opened 'My Reel'")),
           "project.open reads as opened, with spaces in the name");
    expect(line(m::McpTool::ProjectNew, "Untitled").contains(QStringLiteral("created 'Untitled'")),
           "project.new reads as created");
    expect(line(m::McpTool::ProjectClose, "SC020").contains(QStringLiteral("closed 'SC020'")),
           "project.close reads as closed");
    m::McpActivityEntry refused;
    refused.kind = m::McpActivityKind::ToolCall;
    refused.tool = m::McpTool::ProjectNew;
    refused.outcome = m::McpCallOutcome::ToolError;
    refused.summary = "project.new";
    expect(ui::mcppanel::activityLine(refused).contains(QStringLiteral("project.new")),
           "a refused project call keeps the tool name");
}

void testIndicatorAndSound() {
    namespace doc = bloom::document;
    auto project = doc::makeNewProject("Mcp", "Main", bloom::core::RationalTime::fromInteger(10));
    const auto compositionId = project.initialCompositionId;
    doc::Document document(std::move(project.project));
    bloom::commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, compositionId);
    ui::WindowStatusBar bar(session, nullptr);
    bar.show();

    ScriptedMcpPort port;
    ui::McpServerAdapter adapter(port);
    bar.setMcpServer(&adapter);
    expect(!bar.mcpIndicatorVisibleForTest(), "stopped: no indicator");
    show(port, adapter, makeSnapshot(m::McpServerState::Running));
    expect(bar.mcpIndicatorVisibleForTest() && bar.mcpIndicatorTextForTest().contains(QStringLiteral("6767")),
           "running: indicator with the port");
    QSignalSpy reveal(&adapter, &ui::McpServerAdapter::revealRequested);
    bar.findChild<ui::kit::KButton*>(QStringLiteral("windowStatusBarMcp"))->click();
    expect(reveal.count() == 1, "the indicator asks for the panel");
    show(port, adapter, makeSnapshot(m::McpServerState::Stopped));
    expect(!bar.mcpIndicatorVisibleForTest(), "stopped again: hidden");
    auto failed = makeSnapshot(m::McpServerState::Failed);
    failed.failure = m::McpServerFailure{m::McpServerFailureCode::ListenerFailed, 6767, "The server stopped.", {}};
    show(port, adapter, failed);
    expect(bar.mcpIndicatorVisibleForTest(), "failed: indicator");

    // The warning sound plays for a refused start and not for a client connecting.
    struct Player final : ui::SoundPlayer {
        void play(const ui::FeedbackSound, float) noexcept override { ++played; }
        void shutdown() noexcept override {}
        int played = 0;
    };
    auto player = std::make_unique<Player>();
    auto* played = player.get();
    std::chrono::steady_clock::time_point now{std::chrono::seconds{100}};
    ui::UiFeedback feedback(std::move(player), nullptr, [] { return true; }, [&now] { return now; });
    adapter.setFeedback(&feedback);
    m::McpClientView client = ui::mcptest::makeClient(1, "x", "1");
    port.addEvent(m::McpClientConnected{client});
    adapter.drain();
    expect(played->played == 0, "a client connecting makes no sound");
    port.addEvent(m::McpServerStartRefused{*failed.failure});
    adapter.drain();
    expect(played->played == 1, "a refused start plays the warning");
    port.addEvent(m::McpServerListenerFailed{*failed.failure});
    now += std::chrono::seconds(10);
    adapter.drain();
    expect(played->played == 2, "a listener failure plays the warning");
}

} // namespace

// The real service behind the real port: start on an ephemeral free port, see Running, stop.
void testRealService() {
    namespace fs = std::filesystem;
    const auto folder = fs::temp_directory_path() / "bloom-mcp-panel-smoke";
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    bloom::runtime::TaskScheduler scheduler;
    bloom::scripting::LiveSessionHost host;
    const auto freePort = m::testing::McpServerServiceDriver::freePort();
    expect(freePort != 0, "a free port was found");
    {
        m::McpServerService service(m::McpServerServiceDependencies{
            .live = host,
            .exportQueue = {},
            .scheduler = scheduler,
            .tokenFile = folder / "mcp" / "token",
            .captureDirectory = folder / "captures",
            .serverVersion = "test",
            .preferences = {.port = freePort, .autoStart = false},
            .limits = {}});
        ui::McpServerServicePort port(service);
        ui::McpServerAdapter adapter(port);
        const auto pumpUntil = [&](auto predicate) {
            for (int i = 0; i < 2000 && !predicate(); ++i) {
                adapter.drain();
                QTest::qWait(5);
            }
            return predicate();
        };
        expect(pumpUntil([&] { return adapter.snapshot()->token.state == m::McpTokenState::Ready; }),
               "the token loads");
        expect(adapter.start().availability.available, "start is accepted");
        expect(pumpUntil([&] { return adapter.snapshot()->state == m::McpServerState::Running; }),
               "the snapshot shows Running");
        expect(adapter.stop().availability.available, "stop is accepted");
        expect(pumpUntil([&] { return adapter.snapshot()->state == m::McpServerState::Stopped; }),
               "the snapshot shows Stopped");
        service.beginShutdown();
        pumpUntil([&] { return service.quiescent(); });
    }
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        QTest::qWait(1);
    fs::remove_all(folder, ignored);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomMcpPanelTests");
    app.setApplicationName("McpServerPanel");
    ui::kit::installKinetikTheme(app);
    testAdapterDrain();
    testPreferencesPersist();
    testStatusText();
    testAvailability();
    testPortField();
    testToken();
    testRegenerateConfirms();
    testClientsAndActivity();
    testCopyConfig();
    testCompileFailureActivity();
    testCompileFailureFromActivity();
    testProjectActivityLines();
    testIndicatorAndSound();
    testRealService();
    if (gFailures != 0)
        std::cerr << gFailures << " failure(s)\n";
    return gFailures == 0 ? 0 : 1;
}
