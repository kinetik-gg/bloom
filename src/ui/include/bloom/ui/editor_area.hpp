#pragma once

#include <QFrame>
#include <QString>
#include <QStringView>

#include <array>
#include <string>
#include <string_view>

class QEvent;
class QHBoxLayout;
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

// FORMAL AMENDMENT 1 (task C1, after the first report): the footer slot is OPTIONAL, not a
// reserved strip on every panel. An editor widget that also implements this interface -- multiple
// inheritance alongside its usual QWidget base, e.g. `class ViewerEditor final : public QWidget,
// public EditorFooterProvider` -- gets a footer row hosting exactly the widget it hands back;
// one that does not implement it (or returns nullptr) gets no footer row at all, and its body
// extends all the way to the panel's own bottom border (still clipped by the rounded corners).
// Deliberately NOT a new EditorRegistry ABI: EditorDescriptor::create() still returns a single
// QWidget*, and EditorArea discovers this interface with a dynamic_cast on the widget it already
// created, so a footer-less editor pays nothing extra to register or construct.
class EditorFooterProvider {
  public:
    virtual ~EditorFooterProvider() = default;

    // Called once, immediately after EditorArea creates the editor widget in rebuildEditor().
    // Returns the footer widget for EditorArea to host (and take ownership of, by reparenting) in
    // its own footer slot, or nullptr for an editor with no footer to offer. A provider that has
    // already given its footer away (or never has one) returns nullptr on every subsequent call.
    [[nodiscard]] virtual QWidget* takeFooterWidget() = 0;
};

// Task NODES-1: the header's counterpart to EditorFooterProvider, same idempotent "take it once"
// contract and same reasoning for why it is a seam rather than a new EditorRegistry ABI. An editor
// widget that also implements this interface -- the node editor is the first -- gets whatever
// widget it hands back hosted in its own header row, right after the panel switcher and before the
// stretch that pushes the maximize button to the far right; one that does not implement it (or
// returns nullptr) gets nothing extra there, exactly as today. What that widget actually shows --
// how many menus, their contents, and any "too narrow, collapse to one overflow menu" policy -- is
// entirely the provider's own business: EditorArea only reparents it into the header and gives it
// room, the same hands-off relationship it already has with a footer widget.
class EditorHeaderMenuProvider {
  public:
    virtual ~EditorHeaderMenuProvider() = default;

    // Called once, immediately after EditorArea creates the editor widget in rebuildEditor().
    // Returns the header menu widget for EditorArea to host (and take ownership of, by
    // reparenting) in its header row, or nullptr for an editor with no header menus to offer. A
    // provider that has already given its widget away (or never has one) returns nullptr on every
    // subsequent call.
    [[nodiscard]] virtual QWidget* takeHeaderMenuWidget() = 0;
};

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
    // The outer header/content/footer column (task C1, FORMAL AMENDMENT 1): stored so
    // rebuildEditor() can add/remove the OPTIONAL footer widget from it every time the editor
    // changes, not just at construction.
    QVBoxLayout* layout_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    // Task NODES-1: stored so rebuildEditor() can insert/remove the OPTIONAL header menu widget
    // between the panel switcher and the stretch every time the editor changes.
    QHBoxLayout* headerLayout_ = nullptr;
    // task U8, issue #131, fix 4: the header itself opens panelOptionsMenu_ on right-click
    // (contextMenuEvent, routed through EditorArea's own eventFilter -- see watchForActivation());
    // there is no longer a dedicated button that owns the menu.
    QWidget* header_ = nullptr;
    // Self-containment (task C1, item C5, corrected by FORMAL AMENDMENT 1): OPTIONAL. Non-null
    // only while the current editor widget implements EditorFooterProvider and offered a real
    // footer widget (ViewerEditor is the only one today); nullptr for every other editor, which
    // gets no footer row at all -- its content extends to the panel's own bottom border instead.
    // Owned by EditorArea from the moment it is taken (reparented here in rebuildEditor()),
    // rebuilt every time the editor changes.
    QWidget* footer_ = nullptr;
    // Task NODES-1: the header's own OPTIONAL extra, on the same terms as footer_ above -- non-null
    // only while the current editor widget implements EditorHeaderMenuProvider and offered a real
    // widget (the node editor is the only one today). Lives in headerLayout_, between the panel
    // switcher and the stretch; rebuilt every time the editor changes.
    QWidget* headerMenus_ = nullptr;
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
