#include <bloom/ui/mcp_server_adapter.hpp>

#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/ui_feedback.hpp>

#include <QMetaObject>
#include <QSettings>

#include <algorithm>
#include <variant>

namespace bloom::ui {

mcp::McpServerPreferences loadMcpServerPreferences(const QSettings& settings) {
    mcp::McpServerPreferences preferences;
    const int port = settings.value(QStringLiteral("mcp/port"), preferences.port).toInt();
    // A stored value the server would refuse falls back to the default rather than blocking start.
    if (!mcp::validateMcpPort(static_cast<std::uint32_t>(std::max(port, 0))).has_value())
        preferences.port = static_cast<std::uint16_t>(port);
    preferences.autoStart = settings.value(QStringLiteral("mcp/autoStart"), false).toBool();
    return preferences;
}

void saveMcpServerPreferences(QSettings& settings, const mcp::McpServerPreferences& preferences) {
    settings.setValue(QStringLiteral("mcp/port"), static_cast<int>(preferences.port));
    settings.setValue(QStringLiteral("mcp/autoStart"), preferences.autoStart);
    settings.sync();
}

McpServerAdapter::McpServerAdapter(McpServerPort& port, UiFeedback* const feedback,
                                   QObject* const parent)
    : QObject(parent), port_(port), feedback_(feedback) {
    // Start from what the service already holds: a first observe() must not announce history.
    if (const auto snapshot = port_.snapshot())
        revision_ = snapshot->revision;
    if (const auto snapshot = port_.snapshot())
        preferencesRevision_ = snapshot->preferencesRevision;
    lastEventSequence_ = port_.events(0).lastSequence;
}

McpServerAdapter::~McpServerAdapter() {
    if (waiter_.joinable()) {
        waiter_.request_stop();
        waiter_.join();
    }
}

void McpServerAdapter::attach(TaskUiBridge& bridge) {
    connect(&bridge, &TaskUiBridge::resultsAvailable, this, &McpServerAdapter::drain);
}

void McpServerAdapter::startWaiter() {
    if (waiter_.joinable())
        return;
    waiter_ = std::jthread([this](const std::stop_token stop) {
        auto observed = port_.serial();
        while (!stop.stop_requested()) {
            const auto next = port_.waitForChange(observed, stop);
            if (!next.has_value())
                return;
            observed = *next;
            wakeFromWaiter();
        }
    });
}

void McpServerAdapter::wakeFromWaiter() {
    // At most one queued drain: further wakes before it runs are covered by it.
    if (wakePending_.exchange(true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            wakePending_.store(false);
            drain();
        },
        Qt::QueuedConnection);
}

void McpServerAdapter::drain() {
    port_.pump();
    observe();
}

void McpServerAdapter::observe() {
    handleEvents();
    const auto snapshot = port_.snapshot();
    const std::uint64_t revision = snapshot ? snapshot->revision : 0;
    if (revision == revision_)
        return;
    revision_ = revision;
    // Persist the preferences when they moved: the adapter is their only writer (ADR 0034).
    if (snapshot && snapshot->preferencesRevision != preferencesRevision_) {
        preferencesRevision_ = snapshot->preferencesRevision;
        QSettings settings;
        saveMcpServerPreferences(settings, snapshot->preferences);
    }
    Q_EMIT changed();
}

void McpServerAdapter::handleEvents() {
    const auto batch = port_.events(lastEventSequence_);
    for (const auto& event : batch.events) {
        if (event.sequence <= lastEventSequence_)
            continue;
        lastEventSequence_ = event.sequence;
        // A refused start or a failed listener is heard; a client connecting is not.
        if (feedback_ != nullptr &&
            (std::holds_alternative<mcp::McpServerStartRefused>(event.payload) ||
             std::holds_alternative<mcp::McpServerListenerFailed>(event.payload)))
            feedback_->play(FeedbackEvent::Warning);
    }
    lastEventSequence_ = std::max(lastEventSequence_, batch.lastSequence);
}

std::shared_ptr<const mcp::McpServerSnapshot> McpServerAdapter::snapshot() const {
    return port_.snapshot();
}

mcp::McpActivityBatch McpServerAdapter::activity(const std::uint64_t afterId) const {
    return port_.activity(afterId);
}

mcp::McpCommandResult McpServerAdapter::start() {
    auto result = port_.start();
    drain();
    return result;
}

mcp::McpCommandResult McpServerAdapter::stop() {
    auto result = port_.stop();
    drain();
    return result;
}

mcp::McpCommandResult McpServerAdapter::setPort(const std::uint32_t port) {
    auto result = port_.setPort(port);
    drain();
    return result;
}

mcp::McpCommandResult McpServerAdapter::setAutoStart(const bool enabled) {
    auto result = port_.setAutoStart(enabled);
    drain();
    return result;
}

mcp::McpCommandResult McpServerAdapter::regenerateToken() {
    auto result = port_.regenerateToken();
    drain();
    return result;
}

mcp::McpTokenReveal McpServerAdapter::revealToken() {
    auto result = port_.revealToken();
    drain();
    return result;
}

mcp::McpClientConfig McpServerAdapter::clientConfig(const mcp::McpClientKind kind,
                                                    const mcp::McpTokenDisclosure disclosure) const {
    return port_.clientConfig(kind, disclosure);
}

} // namespace bloom::ui
