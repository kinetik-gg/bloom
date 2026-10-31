#pragma once

// The desktop application's side of the MCP live-UI port (ADR 0034, "Owner decisions" item 9):
// MCP is a UI for agents, so every call here does what the artist's own action does, through the
// same controllers -- ProjectHost for open/save/new/close and colour settings, CompositionSession
// for the active composition, selection and playhead, PlaybackController for the transport, the
// WorkspaceHost for revealing editors and the displayed Viewer's own controls for its display
// settings. Nothing here owns project truth or invents a second path.
//
// ONE binding, built once in apps/bloom/main.cpp and handed to McpServerServiceDependencies::liveUi
// through port(). Every member runs on the owner (UI) thread. Session-state members (selection,
// active composition, transport, Viewer) never dirty the project. A refusal is a typed
// mcp::McpUiRefusal with a plain sentence; nothing here opens a dialog.
//
// The window is created after the MCP service, so it is attached later with setWindow(); until then
// the members that need it answer a typed refusal.

#include <bloom/mcp/live_ui_port.hpp>

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace bloom::ui {

class CompositionSession;
class MainWindow;
class RamPreviewController;
class PlaybackController;
class ProjectHost;
enum class ProjectHostOperationOutcome : std::uint8_t;

class McpLiveUiBinding final : public QObject {
    Q_OBJECT

  public:
    // All three are borrowed and outlive the binding.
    McpLiveUiBinding(ProjectHost& project, CompositionSession& session,
                     PlaybackController& playback, QObject* parent = nullptr);
    ~McpLiveUiBinding() override;

    // The window whose workspace and displayed Viewer the members act on. Weak: the window goes
    // before the binding.
    void setWindow(MainWindow* window);
    // The RAM Preview the Viewer button and Numpad 0 run, for transport ram-preview. Weak; without
    // one, ram-preview answers Unbound.
    void setRamPreview(RamPreviewController* ramPreview);

    // The seven members, bound to this object (which must outlive the service).
    [[nodiscard]] mcp::McpLiveUiPort port();

    // The members themselves, public so the port and tests call the same code.
    [[nodiscard]] bool revealComposition(document::CompositionId composition,
                                         std::optional<core::RationalTime> time);
    void projectLifecycle(mcp::McpProjectRequest request,
                          std::function<void(mcp::McpProjectResult)> done);
    [[nodiscard]] mcp::McpUiResult setSelection(const mcp::McpSelectionRequest& request);
    [[nodiscard]] mcp::McpUiResult activateComposition(document::CompositionId composition);
    [[nodiscard]] mcp::McpUiResult transport(const mcp::McpTransportRequest& request);
    [[nodiscard]] mcp::McpUiResult setViewer(const mcp::McpViewerRequest& request);
    [[nodiscard]] mcp::McpUiResult setColorSettings(const document::ColorSettings& settings);

  private:
    struct Pending;

    void onSaveFinished(ProjectHostOperationOutcome outcome, const QString& message);
    void onOpenFinished(ProjectHostOperationOutcome outcome, const QString& message);
    void beginPending(mcp::McpProjectRequestKind kind,
                      std::function<void(mcp::McpProjectResult)> done);
    void finishPending(mcp::McpUiRefusal refusal, const QString& message);
    void settleThenRun(const mcp::McpProjectRequest& request, std::function<void()> run);
    void pollJobs();
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] std::optional<mcp::McpProjectResult>
    refuseUnsettled(const mcp::McpProjectRequest& request) const;
    [[nodiscard]] mcp::McpProjectResult projectResult(mcp::McpUiRefusal refusal,
                                                      const QString& message) const;

    ProjectHost& project_;
    CompositionSession& session_;
    PlaybackController& playback_;
    QPointer<MainWindow> window_;
    QPointer<RamPreviewController> ramPreview_;
    std::unique_ptr<Pending> pending_;
    QTimer* jobsTimer_ = nullptr;
};

} // namespace bloom::ui
