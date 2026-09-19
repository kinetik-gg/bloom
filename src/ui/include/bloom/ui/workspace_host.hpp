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
    // Builds the application's five-area first-run arrangement. The host owns the proportions and
    // topology; callers supply editor identities so the generic split tree remains replaceable.
    void resetToDefaultLayout(const std::array<std::string_view, 4>& topRowEditorIds,
                              std::string_view bottomRowEditorId,
                              std::size_t activeTopRowIndex = 1);

    EditorArea* splitActiveArea(Qt::Orientation orientation);
    EditorArea* splitArea(EditorArea& area, Qt::Orientation orientation,
                          std::string initialEditorId = {}, double newAreaFraction = 0.5);
    [[nodiscard]] bool closeActiveArea();
    [[nodiscard]] bool closeArea(EditorArea& area);

    [[nodiscard]] bool isAreaMaximized() const noexcept;
    void toggleMaximizeActiveArea();

    [[nodiscard]] QByteArray saveLayoutState() const;
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreLayoutState(const QByteArray& state);
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

  signals:
    void activeAreaChanged(EditorArea* area);
    void areaCountChanged(int count);
    void maximizeStateChanged(bool maximized);

  private:
    EditorArea* createArea(std::string_view initialEditorId = {}, QString areaId = {});
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
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreLayoutStateNow(const QByteArray& state);

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
