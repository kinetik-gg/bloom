#include <bloom/ui/mcp_server_editor.hpp>

#include <bloom/ui/mcp_server_text.hpp>
#include "properties_sections.hpp"

#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/mcp_server_adapter.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace bloom::ui {
namespace {

using namespace mcppanel;

constexpr int kActivityCap = 512;
constexpr auto kColumnMinWidth = "mcpColumnMinWidth";

struct ConfigKind {
    mcp::McpClientKind kind;
    const char* id;
};
constexpr std::array<ConfigKind, 4> kConfigKinds{{{mcp::McpClientKind::ClaudeCode, "claudeCode"},
                                                  {mcp::McpClientKind::McpJson, "mcpJson"},
                                                  {mcp::McpClientKind::VsCode, "vsCode"},
                                                  {mcp::McpClientKind::Generic, "other"}}};

kit::KLabel* makeLabel(const QString& name, const kit::TypeRole role, const kit::Color tone,
                       QWidget* const parent) {
    auto* label = new kit::KLabel(QString{}, parent, role);
    label->setObjectName(name);
    label->setTone(tone);
    return label;
}

// An inline reason under a control: the style of the dialogs' field reasons.
// Placed in the field column (label width + gutter), the way Preferences places its helper rows.
QWidget* addReasonRow(QVBoxLayout* const layout, QWidget* const parent, kit::KLabel* const reason) {
    auto* holder = new QWidget(parent);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(kit::px(kit::Spacing::RowPadding) +
                                kit::px(kit::Size::PreferencesLabelWidth) +
                                kit::px(kit::Spacing::PropertyGutter),
                            0, kit::px(kit::Spacing::RowPadding), 0);
    reason->setParent(holder);
    row->addWidget(reason, 1);
    layout->addWidget(holder);
    return holder;
}

kit::KLabel* makeReason(const QString& name, QWidget* const parent) {
    auto* label = makeLabel(name, kit::TypeRole::UiSmall, kit::Color::Error, parent);
    label->setWordWrap(true);
    label->hide();
    return label;
}

// A reason is neutral ink when it only explains why a control is unavailable; the error tone is for
// failures (a refused value, a failed start).
void setReason(kit::KLabel& label, const QString& reason, const bool failure = false) {
    label.setTone(failure ? kit::Color::Error : kit::Color::Muted);
    label.setText(reason);
    label.setVisible(!reason.isEmpty());
}

QWidget* addRow(QVBoxLayout* const layout, QWidget* const parent, const QString& label,
                const std::initializer_list<QWidget*> controls) {
    auto* row = new kit::KPropertyRow(properties::makeRowLabel(label, parent), nullptr, controls,
                                      parent);
    row->setLabelWidth(kit::px(kit::Size::PreferencesLabelWidth));
    layout->addWidget(row);
    return row;
}

// A column cell of the clients list; hidden when the panel is narrower than `minPanelWidth`.
kit::KLabel* column(const QString& text, const int width, const int minPanelWidth,
                    const kit::Color tone, QWidget* const parent) {
    auto* label = new kit::KLabel(QString{}, parent, kit::TypeRole::UiSmall);
    label->setTone(tone);
    label->setElidedText(text);
    label->setToolTip(text);
    label->setFixedWidth(width);
    label->setProperty(kColumnMinWidth, minPanelWidth);
    return label;
}

} // namespace

McpServerEditor::McpServerEditor(McpServerAdapter* const adapter, QWidget* const parent)
    : QWidget(parent), adapter_(adapter) {
    setObjectName(QStringLiteral("mcpServerEditor"));
    setAccessibleName(tr("MCP Server editor"));
    setAccessibleDescription(tr("Start or stop the MCP server and manage its access"));
    build();
    if (adapter_ != nullptr) {
        connect(adapter_, &McpServerAdapter::changed, this, &McpServerEditor::refresh);
        connect(adapter_, &McpServerAdapter::compileFailureNoted, this,
                &McpServerEditor::appendCompileFailures);
        connect(qApp, &QApplication::focusChanged, this, &McpServerEditor::onFocusChanged);
    }
    refresh();
}

void McpServerEditor::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("mcpScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto* surface = new kit::KSurface(nullptr, kit::Color::Background);
    auto* content = new QVBoxLayout(surface);
    content->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::Gutter),
                                kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS));
    content->setSpacing(kit::px(kit::Spacing::Gutter));
    scroll->setWidget(surface);

    // ---- Server.
    auto* server = properties::makeSection(content, surface, QStringLiteral("mcpServerSection"),
                                           tr("Server"), QStringLiteral("mcp/sections/server"));
    server->setResetEnabled(false); // no editable defaults are wired to this section
    auto* body = server->bodyLayout();
    status_ = makeLabel(QStringLiteral("mcpStatus"), kit::TypeRole::Ui, kit::Color::Foreground,
                        server->body());
    startStop_ = new kit::KButton(tr("Start"), server->body());
    startStop_->setObjectName(QStringLiteral("mcpStartStopButton"));
    connect(startStop_, &QAbstractButton::clicked, this, &McpServerEditor::toggleServer);
    addRow(body, server->body(), tr("Status"), {status_});
    addRow(body, server->body(), QString{}, {startStop_});
    startReason_ = makeReason(QStringLiteral("mcpStartReason"), server->body());
    addReasonRow(body, server->body(), startReason_);

    port_ = new kit::KValueField(server->body());
    port_->setObjectName(QStringLiteral("mcpPortField"));
    port_->setAccessibleName(tr("Port"));
    port_->setIntegerMode(true);
    // Wider than the legal range on purpose: validateMcpPort names an out-of-range value instead
    // of the field refusing it silently.
    port_->setIntegerRange(0, 999999);
    port_->setIntegerValue(mcp::kMcpDefaultPort);
    port_->setFixedWidth(kit::px(kit::Size::ViewerChannelWidth));
    connect(port_, &kit::KValueField::integerValueChanged, this,
            [this](const qint64 value) { commitPort(value); });
    addRow(body, server->body(), tr("Port"), {port_});
    portReason_ = makeReason(QStringLiteral("mcpPortReason"), server->body());
    addReasonRow(body, server->body(), portReason_);

    autoStart_ = new kit::KSwitch(server->body());
    autoStart_->setObjectName(QStringLiteral("mcpAutoStartSwitch"));
    autoStart_->setAccessibleName(tr("Start when Bloom opens"));
    auto* words = new kit::KCheckLabel(tr("Start when Bloom opens"), *autoStart_, server->body());
    words->setObjectName(QStringLiteral("mcpAutoStartLabel"));
    words->setTypeRole(kit::TypeRole::UiSmall);
    words->setContentsMargins(kit::px(kit::Spacing::XS), 0, 0, 0);
    connect(autoStart_, &QAbstractButton::toggled, this, [this](const bool on) {
        if (!syncing_ && adapter_ != nullptr)
            (void)adapter_->setAutoStart(on);
    });
    addRow(body, server->body(), QString{}, {autoStart_, words});

    // ---- Access.
    auto* access = properties::makeSection(content, surface, QStringLiteral("mcpAccessSection"),
                                           tr("Access"), QStringLiteral("mcp/sections/access"));
    access->setResetEnabled(false);
    auto* accessBody = access->bodyLayout();
    token_ = makeLabel(QStringLiteral("mcpToken"), kit::TypeRole::Value, kit::Color::Foreground,
                       access->body());
    token_->setTextInteractionFlags(Qt::NoTextInteraction);
    token_->setAccessibleName(tr("Access token"));
    reveal_ = new kit::KButton(tr("Reveal"), access->body());
    reveal_->setObjectName(QStringLiteral("mcpTokenRevealButton"));
    connect(reveal_, &QAbstractButton::clicked, this, &McpServerEditor::reveal);
    copyTokenButton_ = new kit::KButton(tr("Copy"), access->body());
    copyTokenButton_->setObjectName(QStringLiteral("mcpTokenCopyButton"));
    connect(copyTokenButton_, &QAbstractButton::clicked, this, &McpServerEditor::copyToken);
    regenerate_ = new kit::KButton(tr("Regenerate…"), access->body());
    regenerate_->setObjectName(QStringLiteral("mcpTokenRegenerateButton"));
    connect(regenerate_, &QAbstractButton::clicked, this, &McpServerEditor::confirmRegenerate);
    addRow(accessBody, access->body(), tr("Token"), {token_});
    addRow(accessBody, access->body(), QString{}, {reveal_, copyTokenButton_, regenerate_});
    tokenReason_ = makeReason(QStringLiteral("mcpTokenReason"), access->body());
    addReasonRow(accessBody, access->body(), tokenReason_);

    configButton_ = new kit::KMenuButton(access->body());
    configButton_->setObjectName(QStringLiteral("mcpCopyConfigButton"));
    configButton_->setText(tr("Copy client config"));
    configButton_->setAccessibleName(tr("Copy client config"));
    configButton_->setPopupMode(QToolButton::InstantPopup);
    // The Secondary look with a trailing caret, so it reads as a control like its row's buttons.
    configButton_->setProperty("headerMenuButton", false);
    configButton_->setProperty("secondaryMenuButton", true);
    configButton_->setIcon(kit::icon(kit::IconId::CaretDown, kit::IconRole::Chrome));
    configButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    configButton_->setLayoutDirection(Qt::RightToLeft);
    configButton_->setAutoRaise(false);
    auto* menu = kit::makeMenu(configButton_);
    menu->setObjectName(QStringLiteral("mcpCopyConfigMenu"));
    for (const auto disclosure :
         {mcp::McpTokenDisclosure::IncludeToken, mcp::McpTokenDisclosure::EnvironmentPlaceholder}) {
        if (disclosure == mcp::McpTokenDisclosure::EnvironmentPlaceholder)
            menu->addSeparator();
        for (const auto& entry : kConfigKinds) {
            const bool placeholder = disclosure == mcp::McpTokenDisclosure::EnvironmentPlaceholder;
            auto* action = menu->addAction(
                placeholder ? tr("%1 with ${BLOOM_MCP_TOKEN} placeholder")
                                  .arg(clientKindText(entry.kind))
                            : clientKindText(entry.kind));
            action->setObjectName(QStringLiteral("mcpCopyConfig_%1_%2")
                                      .arg(QLatin1StringView(entry.id),
                                           placeholder ? QStringLiteral("placeholder")
                                                       : QStringLiteral("token")));
            connect(action, &QAction::triggered, this,
                    [this, kind = entry.kind, disclosure] { copyConfig(kind, disclosure); });
        }
    }
    configButton_->setMenu(menu);
    addRow(accessBody, access->body(), QString{}, {configButton_});

    // ---- Connected clients.
    auto* clients =
        properties::makeSection(content, surface, QStringLiteral("mcpClientsSection"),
                                tr("Connected clients"), QStringLiteral("mcp/sections/clients"));
    clients->setResetEnabled(false);
    clientsBody_ = new QWidget(clients->body());
    clientsBody_->setObjectName(QStringLiteral("mcpClientsList"));
    clientsLayout_ = new QVBoxLayout(clientsBody_);
    clientsLayout_->setContentsMargins(0, 0, 0, 0);
    clientsLayout_->setSpacing(0);
    clients->bodyLayout()->addWidget(clientsBody_);
    project_ = makeLabel(QStringLiteral("mcpProjectLine"), kit::TypeRole::UiSmall,
                         kit::Color::Muted, clients->body());
    project_->setWordWrap(true);
    project_->hide();
    clients->bodyLayout()->addWidget(project_);

    // ---- Activity.
    auto* activity =
        properties::makeSection(content, surface, QStringLiteral("mcpActivitySection"),
                                tr("Activity"), QStringLiteral("mcp/sections/activity"));
    activity->setResetEnabled(false);
    activity_ = new QListWidget(activity->body());
    activity_->setObjectName(QStringLiteral("mcpActivityList"));
    activity_->setAccessibleName(tr("MCP activity"));
    activity_->setFrameShape(QFrame::NoFrame);
    activity_->setFont(kit::font(kit::TypeRole::UiSmall));
    activity_->setSelectionMode(QAbstractItemView::NoSelection);
    activity_->setFocusPolicy(Qt::NoFocus);
    activity_->setMinimumHeight(kit::px(kit::Size::NodeCardWidth));
    activity_->setTextElideMode(Qt::ElideRight);
    activity->bodyLayout()->addWidget(activity_);
    activityEmpty_ = makeLabel(QStringLiteral("mcpNoActivity"), kit::TypeRole::UiSmall,
                               kit::Color::Muted, activity->body());
    activityEmpty_->setText(tr("Activity appears here while the server is running."));
    activity->bodyLayout()->addWidget(activityEmpty_);
    syncActivityEmpty();
    content->addStretch(1);

    revealTimer_ = new QTimer(this);
    revealTimer_->setObjectName(QStringLiteral("mcpRevealTimer"));
    revealTimer_->setSingleShot(true);
    revealTimer_->setInterval(kRevealMilliseconds);
    connect(revealTimer_, &QTimer::timeout, this, &McpServerEditor::hideToken);
}

void McpServerEditor::refresh() {
    const auto snapshot = adapter_ != nullptr ? adapter_->snapshot() : nullptr;
    if (!snapshot) {
        status_->setElidedText(tr("The MCP server is not available in this build."));
        for (QWidget* control : {static_cast<QWidget*>(startStop_), static_cast<QWidget*>(port_),
                                 static_cast<QWidget*>(autoStart_), static_cast<QWidget*>(reveal_),
                                 static_cast<QWidget*>(copyTokenButton_),
                                 static_cast<QWidget*>(regenerate_),
                                 static_cast<QWidget*>(configButton_)})
            control->setEnabled(false);
        return;
    }
    shownRevision_ = snapshot->revision;
    using C = mcp::McpServerCommand;
    const bool running = snapshot->state == mcp::McpServerState::Running;

    status_->setElidedText(statusText(*snapshot));
    status_->setTone(snapshot->failure.has_value() && !running ? kit::Color::Error
                                                               : kit::Color::Foreground);

    // One button: Stop while the server is up, Start otherwise.
    const auto& toggle = snapshot->commands[running || snapshot->state == mcp::McpServerState::Stopping
                                                ? C::Stop
                                                : C::Start];
    const bool stopShown = running || snapshot->state == mcp::McpServerState::Stopping;
    startStop_->setText(stopShown ? tr("Stop") : tr("Start"));
    startStop_->setVariant(stopShown ? kit::KButton::Variant::Secondary
                                     : kit::KButton::Variant::Primary);
    startStop_->setEnabled(toggle.available);
    startStop_->setToolTip(toggle.available ? QString{} : qs(toggle.reason));
    setReason(*startReason_, toggle.available ? QString{} : qs(toggle.reason));

    // Port: editable only while stopped; the reason is shown, never just a disabled field.
    const auto& setPortAvailability = snapshot->commands[C::SetPort];
    {
        syncing_ = true;
        const QSignalBlocker block(port_);
        if (portError_.isEmpty())
            port_->setIntegerValue(snapshot->preferences.port);
        const QSignalBlocker blockSwitch(autoStart_);
        autoStart_->setChecked(snapshot->preferences.autoStart);
        syncing_ = false;
    }
    const bool portEditable = setPortAvailability.available ||
                              setPortAvailability.code == mcp::McpUnavailableCode::PortOutOfRange;
    port_->setEnabled(portEditable);
    port_->setToolTip(portEditable ? QString{} : qs(setPortAvailability.reason));
    QString reason = portError_;
    bool portFailure = !reason.isEmpty();
    if (reason.isEmpty() && !portEditable)
        reason = qs(setPortAvailability.reason);
    if (reason.isEmpty() && snapshot->failure.has_value() && !running &&
        snapshot->failure->port != 0)
    {
        reason = qs(snapshot->failure->reason);
        portFailure = true;
    }
    setReason(*portReason_, reason, portFailure);
    autoStart_->setEnabled(snapshot->commands[C::SetAutoStart].available);
    autoStart_->setToolTip(snapshot->commands[C::SetAutoStart].available
                               ? QString{}
                               : qs(snapshot->commands[C::SetAutoStart].reason));

    refreshToken(*snapshot);
    refreshConfigMenu(*snapshot);
    refreshClients(*snapshot);
    if (!snapshot->project.bound && !snapshot->project.reason.empty()) {
        project_->setText(qs(snapshot->project.reason));
        project_->show();
    } else {
        project_->hide();
    }
    refreshActivity();
    appendCompileFailures();
}

void McpServerEditor::refreshToken(const mcp::McpServerSnapshot& snapshot) {
    using C = mcp::McpServerCommand;
    const auto& tokenView = snapshot.token;
    if (tokenView.state != mcp::McpTokenState::Ready)
        hideToken();
    switch (tokenView.state) {
    case mcp::McpTokenState::Loading:
        token_->setElidedText(tr("Loading…"));
        break;
    case mcp::McpTokenState::Regenerating:
        token_->setElidedText(tr("Regenerating…"));
        break;
    case mcp::McpTokenState::Unavailable:
        token_->setElidedText(tr("Unavailable"));
        break;
    case mcp::McpTokenState::Ready:
        token_->setElidedText(revealedToken_.isEmpty() ? qs(tokenView.masked) : revealedToken_);
        break;
    }
    const auto& revealAvailability = snapshot.commands[C::RevealToken];
    const auto& regenerateAvailability = snapshot.commands[C::RegenerateToken];
    reveal_->setEnabled(revealAvailability.available);
    reveal_->setText(revealedToken_.isEmpty() ? tr("Reveal") : tr("Hide"));
    copyTokenButton_->setEnabled(revealAvailability.available);
    regenerate_->setEnabled(regenerateAvailability.available);
    for (auto* button : {reveal_, copyTokenButton_})
        button->setToolTip(revealAvailability.available ? QString{}
                                                        : qs(revealAvailability.reason));
    regenerate_->setToolTip(regenerateAvailability.available ? QString{}
                                                             : qs(regenerateAvailability.reason));
    QString reason;
    if (!revealAvailability.available)
        reason = qs(revealAvailability.reason);
    else if (!regenerateAvailability.available)
        reason = qs(regenerateAvailability.reason);
    if (reason.isEmpty() && tokenView.state == mcp::McpTokenState::Unavailable)
        reason = qs(tokenView.reason);
    setReason(*tokenReason_, reason);
}

void McpServerEditor::refreshConfigMenu(const mcp::McpServerSnapshot& snapshot) {
    const auto& availability = snapshot.commands[mcp::McpServerCommand::CopyClientConfig];
    for (auto* action : configButton_->menu()->actions()) {
        if (action->isSeparator())
            continue;
        const bool withToken = action->objectName().endsWith(QStringLiteral("_token"));
        const bool enabled = !withToken || availability.available;
        action->setEnabled(enabled);
        action->setToolTip(enabled ? QString{} : qs(availability.reason));
    }
    configButton_->setEnabled(true);
}

void McpServerEditor::refreshClients(const mcp::McpServerSnapshot& snapshot) {
    while (auto* item = clientsLayout_->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const int wide = kit::px(kit::Spacing::XXL) * 3;
    const int narrow = kit::px(kit::Spacing::XXL) * 2;
    const int toolWidth = kit::px(kit::Spacing::XXL) * 4;
    const auto addRowOf = [&](const QString& name, const QString& version, const QString& since,
                              const QString& last, const QString& inFlight, const QString& tool,
                              const bool header, const QString& rowName) {
        auto* row = new kit::KRow(clientsBody_);
        row->setObjectName(rowName);
        const auto tone = header ? kit::Color::Muted : kit::Color::Foreground;
        auto* nameLabel = new kit::KLabel(QString{}, row, kit::TypeRole::UiSmall);
        nameLabel->setTone(tone);
        nameLabel->setElidedText(name);
        nameLabel->setToolTip(name);
        nameLabel->setObjectName(QStringLiteral("mcpClientName"));
        auto* versionLabel = column(version, narrow, 480, tone, row);
        versionLabel->setObjectName(QStringLiteral("mcpClientVersion"));
        auto* sinceLabel = column(since, wide, 0, tone, row);
        sinceLabel->setObjectName(QStringLiteral("mcpClientSince"));
        auto* lastLabel = column(last, wide, 0, tone, row);
        lastLabel->setObjectName(QStringLiteral("mcpClientLastActivity"));
        auto* flightLabel = column(inFlight, narrow, 360, tone, row);
        flightLabel->setObjectName(QStringLiteral("mcpClientInFlight"));
        auto* toolLabel = column(tool, toolWidth, 640, tone, row);
        toolLabel->setObjectName(QStringLiteral("mcpClientLastTool"));
        row->setCells({}, nameLabel, {versionLabel, sinceLabel, lastLabel, flightLabel, toolLabel});
        row->setFixedHeight(kit::px(kit::Size::AssetRow));
        clientsLayout_->addWidget(row);
    };
    if (snapshot.clients.empty()) {
        auto* empty = makeLabel(QStringLiteral("mcpNoClients"), kit::TypeRole::UiSmall,
                                kit::Color::Muted, clientsBody_);
        empty->setText(snapshot.state == mcp::McpServerState::Running
                           ? tr("No clients connected.")
                           : tr("Clients appear here while the server is running."));
        clientsLayout_->addWidget(empty);
        return;
    }
    addRowOf(tr("Client"), tr("Version"), tr("Connected"), tr("Last activity"), tr("In flight"),
             tr("Last tool"), true, QStringLiteral("mcpClientsHeader"));
    for (const auto& client : snapshot.clients)
        addRowOf(clientName(client.info), qs(client.info.version), timeText(client.connectedAt),
                 timeText(client.lastActivityAt), QString::number(client.inFlight),
                 toolText(client.lastTool), false, QStringLiteral("mcpClientRow"));
    layoutClientColumns();
}

void McpServerEditor::layoutClientColumns() {
    if (clientsBody_ == nullptr)
        return;
    for (auto* label : clientsBody_->findChildren<kit::KLabel*>()) {
        const auto minimum = label->property(kColumnMinWidth);
        if (minimum.isValid())
            label->setVisible(width() >= minimum.toInt());
    }
}

void McpServerEditor::refreshActivity() {
    if (adapter_ == nullptr)
        return;
    const auto snapshot = adapter_->snapshot();
    if (snapshot && snapshot->lastActivityId == lastActivityId_)
        return;
    const auto batch = adapter_->activity(lastActivityId_);
    if (batch.gap && activity_->count() > 0 &&
        activity_->item(0)->data(Qt::UserRole).toBool() == false) {
        auto* trimmed = new QListWidgetItem(tr("Earlier activity was trimmed."));
        trimmed->setData(Qt::UserRole, true);
        activity_->insertItem(0, trimmed);
    } else if (batch.gap && activity_->count() == 0) {
        auto* trimmed = new QListWidgetItem(tr("Earlier activity was trimmed."));
        trimmed->setData(Qt::UserRole, true);
        activity_->addItem(trimmed);
    }
    auto* bar = activity_->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 2;
    for (const auto& entry : batch.entries) {
        if (entry.id <= lastActivityId_)
            continue;
        auto* item = new QListWidgetItem(activityLine(entry));
        item->setToolTip(item->text());
        activity_->addItem(item);
    }
    while (activity_->count() > kActivityCap)
        delete activity_->takeItem(activity_->item(0)->data(Qt::UserRole).toBool() ? 1 : 0);
    lastActivityId_ = std::max(batch.lastId, lastActivityId_);
    if (atBottom)
        activity_->scrollToBottom();
    syncActivityEmpty();
}

void McpServerEditor::appendCompileFailures() {
    if (adapter_ == nullptr)
        return;
    for (const auto& note : adapter_->compileFailures()) {
        if (note.id <= lastCompileFailureId_)
            continue;
        lastCompileFailureId_ = note.id;
        auto* item = new QListWidgetItem(compileFailureLine(note));
        item->setToolTip(item->text());
        item->setForeground(kit::color(kit::Color::Error));
        activity_->addItem(item);
    }
    activity_->scrollToBottom();
    syncActivityEmpty();
}

void McpServerEditor::syncActivityEmpty() {
    const bool empty = activity_->count() == 0;
    activity_->setVisible(!empty);
    activityEmpty_->setVisible(empty);
}

void McpServerEditor::toggleServer() {
    if (adapter_ == nullptr)
        return;
    const auto snapshot = adapter_->snapshot();
    if (snapshot && snapshot->state == mcp::McpServerState::Running)
        (void)adapter_->stop();
    else
        (void)adapter_->start();
}

void McpServerEditor::setPortReason(const QString& reason) {
    portError_ = reason;
    setReason(*portReason_, reason, !reason.isEmpty());
}

void McpServerEditor::commitPort(const std::int64_t value) {
    if (adapter_ == nullptr || syncing_)
        return;
    if (const auto refusal = mcp::validateMcpPort(static_cast<std::uint32_t>(value))) {
        setPortReason(qs(refusal->reason));
        return;
    }
    const auto result = adapter_->setPort(static_cast<std::uint32_t>(value));
    if (!result.accepted) {
        setPortReason(qs(result.availability.reason));
        return;
    }
    setPortReason({});
    refresh();
}

void McpServerEditor::reveal() {
    if (adapter_ == nullptr)
        return;
    if (!revealedToken_.isEmpty()) {
        hideToken();
        return;
    }
    const auto result = adapter_->revealToken();
    if (!result.availability.available) {
        setReason(*tokenReason_, qs(result.availability.reason));
        return;
    }
    revealedToken_ = qs(result.token);
    revealTimer_->start();
    refresh();
}

void McpServerEditor::hideToken() {
    if (revealedToken_.isEmpty())
        return;
    revealedToken_.clear();
    revealTimer_->stop();
    if (adapter_ != nullptr)
        refresh();
}

void McpServerEditor::copyToken() {
    if (adapter_ == nullptr)
        return;
    const auto result = adapter_->revealToken();
    if (!result.availability.available) {
        setReason(*tokenReason_, qs(result.availability.reason));
        return;
    }
    QGuiApplication::clipboard()->setText(qs(result.token));
}

void McpServerEditor::confirmRegenerate() {
    if (adapter_ == nullptr)
        return;
    kit::KConfirmSpec spec;
    spec.title = tr("Regenerate token");
    spec.objectName = QStringLiteral("mcpRegenerateConfirm");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = tr("Regenerate the access token?");
    spec.detail = tr("Every connected client will be disconnected and needs the new token.");
    spec.destructiveText = tr("Regenerate");
    spec.rejectText = tr("Cancel");
    spec.defaultRole = kit::KDialog::Role::Reject;
    kit::confirm(*this, spec, [this](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Destructive || adapter_ == nullptr)
            return;
        hideToken();
        const auto result = adapter_->regenerateToken();
        if (!result.accepted)
            setReason(*tokenReason_, qs(result.availability.reason));
    });
}

void McpServerEditor::copyConfig(const mcp::McpClientKind kind,
                                 const mcp::McpTokenDisclosure disclosure) {
    if (adapter_ == nullptr)
        return;
    const auto config = adapter_->clientConfig(kind, disclosure);
    if (!config.availability.available) {
        setReason(*tokenReason_, qs(config.availability.reason));
        return;
    }
    QGuiApplication::clipboard()->setText(qs(config.text));
}

void McpServerEditor::onFocusChanged(QWidget* const /*old*/, QWidget* const now) {
    if (revealedToken_.isEmpty())
        return;
    if (now == nullptr || (now != this && !isAncestorOf(now)))
        hideToken();
}

void McpServerEditor::hideEvent(QHideEvent* const event) {
    hideToken();
    QWidget::hideEvent(event);
}

void McpServerEditor::resizeEvent(QResizeEvent* const event) {
    QWidget::resizeEvent(event);
    layoutClientColumns();
}

bool registerMcpServerEditor(EditorRegistry& registry, McpServerAdapter* const adapter) {
    return registry.registerEditor(
        {.id = "bloom.mcp-server",
         .displayName = McpServerEditor::tr("MCP Server"),
         .create = [adapter](QWidget* parent) { return new McpServerEditor(adapter, parent); }});
}

} // namespace bloom::ui
