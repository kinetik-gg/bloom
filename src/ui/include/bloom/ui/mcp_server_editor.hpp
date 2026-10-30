#pragma once

// The MCP Server panel (ADR 0034, section 7): start or stop the in-app MCP server, choose its
// port, manage the access token, copy client configuration and watch clients and activity. A
// registered editor type, id bloom.mcp-server. It holds no server state: it reads the snapshot
// through the application's McpServerAdapter, issues commands through it and never pumps.

#include <bloom/mcp/server_service.hpp>

#include <QString>
#include <QWidget>

#include <cstdint>

class QListWidget;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace bloom::ui::kit {
class KButton;
class KLabel;
class KMenuButton;
class KSwitch;
class KValueField;
} // namespace bloom::ui::kit

namespace bloom::ui {

class EditorRegistry;
class McpServerAdapter;

class McpServerEditor final : public QWidget {
    Q_OBJECT

  public:
    // `adapter` may be null (a build without the server): the panel then says so.
    explicit McpServerEditor(McpServerAdapter* adapter, QWidget* parent = nullptr);

    // Seconds the token stays revealed when the panel keeps focus.
    static constexpr int kRevealMilliseconds = 30000;

  protected:
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void build();
    void refresh();
    void refreshClients(const mcp::McpServerSnapshot& snapshot);
    void refreshActivity();
    void appendCompileFailures();
    void syncActivityEmpty();
    void refreshToken(const mcp::McpServerSnapshot& snapshot);
    void refreshConfigMenu(const mcp::McpServerSnapshot& snapshot);
    void toggleServer();
    void commitPort(std::int64_t value);
    void setPortReason(const QString& reason);
    void reveal();
    void copyToken();
    void hideToken();
    void confirmRegenerate();
    void copyConfig(mcp::McpClientKind kind, mcp::McpTokenDisclosure disclosure);
    void onFocusChanged(QWidget* old, QWidget* now);
    void layoutClientColumns();

    McpServerAdapter* adapter_ = nullptr;
    kit::KLabel* status_ = nullptr;
    kit::KButton* startStop_ = nullptr;
    kit::KLabel* startReason_ = nullptr;
    kit::KValueField* port_ = nullptr;
    kit::KLabel* portReason_ = nullptr;
    kit::KSwitch* autoStart_ = nullptr;
    kit::KLabel* token_ = nullptr;
    kit::KLabel* tokenReason_ = nullptr;
    kit::KButton* reveal_ = nullptr;
    kit::KButton* copyTokenButton_ = nullptr;
    kit::KButton* regenerate_ = nullptr;
    kit::KMenuButton* configButton_ = nullptr;
    QWidget* clientsBody_ = nullptr;
    QVBoxLayout* clientsLayout_ = nullptr;
    kit::KLabel* project_ = nullptr;
    QListWidget* activity_ = nullptr;
    kit::KLabel* activityEmpty_ = nullptr;
    QTimer* revealTimer_ = nullptr;
    QString revealedToken_;
    QString portError_;
    bool syncing_ = false;
    std::uint64_t lastActivityId_ = 0;
    std::uint64_t lastCompileFailureId_ = 0;
    std::uint64_t shownRevision_ = ~std::uint64_t{0};
    bool narrow_ = false;
};

[[nodiscard]] bool registerMcpServerEditor(EditorRegistry& registry, McpServerAdapter* adapter);

} // namespace bloom::ui
