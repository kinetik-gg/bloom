#pragma once

#include <bloom/ui/native_surface_retirement.hpp>

#include <QByteArray>
#include <QFrame>
#include <QHash>
#include <QList>
#include <QPointer>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class QSettings;
class QSplitter;
class QString;
class QVBoxLayout;

namespace bloom::ui {

class EditorArea;
class EditorRegistry;

enum class WorkspaceLayoutRestoreResult {
    Restored,
    Missing,
    Invalid,
    UnsupportedVersion,
    // A live native surface must retire before the restored tree may replace the current one. No
    // widget-tree state or selection truth has changed; the restore completes asynchronously.
    Deferred,
};

class WorkspaceHost final : public QFrame {
    Q_OBJECT

  public:
    explicit WorkspaceHost(const EditorRegistry& editorRegistry, QWidget* parent = nullptr);

    [[nodiscard]] EditorArea* activeArea() const noexcept;
    [[nodiscard]] int areaCount() const;
    void setActiveArea(EditorArea* area);
    void resetToSingleArea(std::string_view editorId = {});
    // Builds the application's five-area first-run arrangement: a full-height right column (Assets
    // over Properties) beside a left region whose top row is Viewer | Nodes and whose bottom is the
    // Timeline. The host owns the proportions and topology; callers supply editor identities so the
    // generic split tree remains replaceable. `activeIndex` selects the active area in the order
    // Viewer (0), Nodes (1), Assets (2), Timeline (3), Properties (4).
    void resetToDefaultLayout(std::string_view viewerEditorId, std::string_view nodesEditorId,
                              std::string_view assetsEditorId, std::string_view timelineEditorId,
                              std::string_view propertiesEditorId, std::size_t activeIndex = 0);

    // Shows the editor `editorId` and makes its area active (and focused): the area that already
    // hosts it, or a new area split off the Timeline's (otherwise the active one), below it.
    // Per-editor policy: "bloom.export" is never split off -- with no Export open it replaces the
    // Timeline's editor in place, and with no Timeline either it takes a new panel along the
    // bottom of the workspace. A maximized area is
    // restored first so the revealed editor is visible. Returns the area, or nullptr when none could
    // be shown (the area limit, or a native surface still retiring).
    EditorArea* revealEditor(std::string_view editorId, Qt::Orientation orientation = Qt::Vertical,
                             double newAreaFraction = 0.4);

    EditorArea* splitActiveArea(Qt::Orientation orientation);
    EditorArea* splitArea(EditorArea& area, Qt::Orientation orientation,
                          std::string initialEditorId = {}, double newAreaFraction = 0.5);
    [[nodiscard]] bool closeActiveArea();
    [[nodiscard]] bool closeArea(EditorArea& area);

    [[nodiscard]] bool isAreaMaximized() const noexcept;
    void toggleMaximizeActiveArea();

    [[nodiscard]] QByteArray saveLayoutState() const;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreLayoutState(const QByteArray& state);
    // Rearranges the panels into `layoutState` (the saveLayoutState() format) WITHOUT rebuilding the
    // ones it keeps: each area of the new tree is taken from the current tree when one hosts the
    // same editor type (so that editor's session state -- zoom, scroll, selection -- survives),
    // and created otherwise; areas the new tree has no place for close. Validation runs before any
    // widget moves, so an invalid state changes nothing.
    [[nodiscard]] WorkspaceLayoutRestoreResult applyArrangement(const QByteArray& layoutState);
    void persistLayout(QSettings& settings, const QString& key) const;
    [[nodiscard]] WorkspaceLayoutRestoreResult restorePersistedLayout(QSettings& settings,
                                                                      const QString& key);

    // True while a workspace mutation is waiting for one or more live native surfaces to retire.
    // Until this clears, the split tree, widget parents, area count, and active area are unchanged.
    [[nodiscard]] bool isNativeSurfaceMutationPending() const noexcept;
    // Honest diagnostic from the last refused mutation (empty when none).
    [[nodiscard]] const std::string& lastNativeSurfaceDiagnostic() const noexcept;

    // Every live editor native surface in the current tree, for the shutdown retirement gate. The
    // ApplicationShutdownCoordinator consumes this through a plain std::function seam, so
    // WorkspaceHost does not gain a second QObject base (which would desynchronize the frozen
    // moc-generated vtable).
    [[nodiscard]] std::vector<EditorNativeSurface*> liveNativeSurfaces() const;
    // After task/native retirement: visit the existing tree without allocating a surface list.
    void releasePackedFramesForShutdown();

  signals:
    void activeAreaChanged(EditorArea* area);
    void areaCountChanged(int count);
    void maximizeStateChanged(bool maximized);
    // The arrangement changed: a split, close, divider drag, editor swap or a restored layout.
    // Emitted for artist edits and for programmatic changes alike; the preset picker compares.
    void layoutChanged();

  private:
    EditorArea* createArea(std::string_view initialEditorId = {}, QString areaId = {});
    EditorArea* spawnBottomArea(std::string_view editorId, double fraction);
    QSplitter* createSplitter(Qt::Orientation orientation) const;
    void replaceRoot(QWidget* newRoot);
    void updateAreaControls();
    void restoreMaximizedArea();

    // Retires every live native surface in the current tree before running `commit`,
    // all-or-nothing. CPU-only trees commit synchronously. A pending gate rejects further
    // mutations.
    NativeSurfaceRetirementGate::StartStatus
    beginWorkspaceMutation(NativeSurfaceRetirementGate::Commit commit,
                           NativeSurfaceRetirementGate::Finish finish);
    [[nodiscard]] std::vector<EditorNativeSurface*> collectNativeSurfaces() const;
    void onWorkspaceMutationFinished(const NativeSurfaceRetirementGate::Result& result);
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreLayoutStateNow(const QByteArray& state,
                                                                     bool reuseAreas = false);

    const EditorRegistry& editorRegistry_;
    QVBoxLayout* rootLayout_ = nullptr;
    QWidget* rootWidget_ = nullptr;
    QPointer<EditorArea> activeArea_;
    QPointer<EditorArea> maximizedArea_;
    QHash<QSplitter*, QList<int>> preMaximizeSizes_;
    NativeSurfaceRetirementGate surfaceRetirementGate_;
    std::string lastNativeSurfaceDiagnostic_;
};

} // namespace bloom::ui
