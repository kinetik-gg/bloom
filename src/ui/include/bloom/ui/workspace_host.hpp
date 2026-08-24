#pragma once

#include <QByteArray>
#include <QFrame>
#include <QHash>
#include <QList>
#include <QPointer>

#include <string>

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
