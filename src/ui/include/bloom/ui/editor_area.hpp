#pragma once

#include <QFrame>
#include <QString>
#include <QStringView>

#include <array>
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
    void layoutCornerMasks();

    const EditorRegistry& editorRegistry_;
    QString areaId_;
    kit::KPanelSwitcher* editorPicker_ = nullptr;
    QWidget* editorWidget_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    // task U8, issue #131, fix 4: the header itself opens panelOptionsMenu_ on right-click
    // (contextMenuEvent, routed through EditorArea's own eventFilter -- see watchForActivation());
    // there is no longer a dedicated button that owns the menu.
    QWidget* header_ = nullptr;
    // Self-containment (task C1, item C5): a footer strip mirroring the header -- Size::Control
    // tall, Surface background, the same Border hairline -- so every panel has both a header AND
    // a footer that belong to it, not just a header. Empty for now: see layoutCornerMasks()'s own
    // comment and this task's final report for why an existing editor's own bottom bar (the
    // viewer's status bar, the timeline's transport) is not moved into this slot yet.
    QWidget* footer_ = nullptr;
    // The real clip this container needs (task C1, item C5; owner: "cut rounded corners because
    // the background is not clipped by the panel"): four small overlay widgets, one per corner,
    // stacked on top of the header/content/footer children and painted last. Each one fills the
    // little wedge outside the frame's own Radius::Panel curve with Color::Background -- the one
    // color every rounded panel corner always reveals -- so a header/content/footer's own square
    // corner can never bleed past the curve, regardless of resize, HiDPI, or what a given editor's
    // content widget paints. See editor_area.cpp's anonymous-namespace PanelCornerMask.
    std::array<QWidget*, 4> cornerMasks_{};
    QMenu* contextMenu_ = nullptr;
    QToolButton* maximizeButton_ = nullptr;
    bool active_ = false;
};

} // namespace bloom::ui
