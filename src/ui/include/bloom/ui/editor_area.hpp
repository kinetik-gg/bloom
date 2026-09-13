#pragma once

#include <QFrame>
#include <QString>
#include <QStringView>

#include <string>
#include <string_view>

class QEvent;
class QMenu;
class QObject;
class QResizeEvent;
class QToolButton;
class QVBoxLayout;

namespace bloom::ui::kit {
class KPanelSwitcher;
} // namespace bloom::ui::kit

namespace bloom::ui {

class EditorRegistry;

class EditorArea final : public QFrame {
    Q_OBJECT

  public:
    explicit EditorArea(const EditorRegistry& registry, std::string_view initialEditorId = {},
                        QString areaId = {}, QWidget* parent = nullptr);

    [[nodiscard]] const QString& areaId() const noexcept;
    [[nodiscard]] static bool isValidAreaId(QStringView areaId);
    [[nodiscard]] std::string editorId() const;
    [[nodiscard]] bool setEditorId(std::string_view editorId);

    void setAreaActive(bool active);
    [[nodiscard]] bool isAreaActive() const noexcept;
    void setSplitEnabled(bool enabled);
    void setCloseEnabled(bool enabled);
    void setMaximizedAppearance(bool maximized);

  signals:
    void activationRequested(EditorArea* area);
    void splitRequested(EditorArea* area, Qt::Orientation orientation);
    void closeRequested(EditorArea* area);
    void maximizeRequested(EditorArea* area);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void rebuildEditor(int editorIndex);
    int addUnavailableEditor(std::string_view editorId);
    void watchForActivation(QWidget* widget);
    void updateRoundedMask();

    const EditorRegistry& editorRegistry_;
    QString areaId_;
    kit::KPanelSwitcher* editorPicker_ = nullptr;
    QWidget* editorWidget_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    // task U8, issue #131, fix 4: the header itself opens panelOptionsMenu_ on right-click
    // (contextMenuEvent, routed through EditorArea's own eventFilter -- see watchForActivation());
    // there is no longer a dedicated button that owns the menu.
    QWidget* header_ = nullptr;
    QMenu* contextMenu_ = nullptr;
    QToolButton* maximizeButton_ = nullptr;
    bool active_ = false;
};

} // namespace bloom::ui
