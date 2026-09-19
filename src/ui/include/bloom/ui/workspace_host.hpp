#pragma once

#include <QByteArray>
#include <QFrame>
#include <QHash>
#include <QList>
#include <QPointer>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

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

    const EditorRegistry& editorRegistry_;
    QVBoxLayout* rootLayout_ = nullptr;
    QWidget* rootWidget_ = nullptr;
    QPointer<EditorArea> activeArea_;
    QPointer<EditorArea> maximizedArea_;
    QHash<QSplitter*, QList<int>> preMaximizeSizes_;
};

} // namespace bloom::ui
