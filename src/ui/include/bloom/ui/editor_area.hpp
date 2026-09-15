#pragma once

#include <QFrame>
#include <QSize>
#include <QString>
#include <QStringView>

#include <QPointer>
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QEvent;
class QHBoxLayout;
class QMenu;
class QObject;
class QResizeEvent;
class QToolButton;
class QVBoxLayout;

namespace bloom::ui::kit {
class KDropdown;
} // namespace bloom::ui::kit

namespace bloom::ui::kit {
class KPanelFrame;
}
namespace bloom::ui {

class EditorRegistry;

// Controls and actions are declared by panels; EditorArea owns materialization and layout.
struct EditorChromeRowSpec {
    struct Entry {
        QWidget* control;
        bool menu = false;
        bool visible = true;
        bool trailing = false;
    };
    QString objectName;
    QString overflowButtonName;
    QString overflowMenuName;
    std::vector<Entry> entries;
    QPointer<QWidget> host;
    QWidget* owner = nullptr;
    bool trailing = false;
    void addWidget(QWidget* control);
    void addStretch(int = 1) { trailing = true; }
    QToolButton* addMenuButton(const QString& title, QMenu* menu, const QString& name = {},
                               bool visible = true);
    QToolButton* addMenu(QMenu* menu, const QString& name, bool visible = true);
    QMenu* addMenu(const QString& title);
    void addTopLevelMenu(const QString& title, QMenu* menu) { addMenuButton(title, menu); }
};
struct EditorCanvasChromeSpec {
    QString objectName;
    QString gutterName;
    QWidget* strip = nullptr;
    QWidget* canvas = nullptr;
    int gutterWidth = 0;
    int leadingWidth = 0;
    QString leadingName;
};
struct EditorChromeSpec {
    EditorChromeRowSpec header;
    EditorChromeRowSpec footer;
    QWidget* headerCanvas = nullptr;
    std::function<int()> splitPosition;
    std::function<void()> hosted;
};
class EditorChromeProvider {
  public:
    virtual ~EditorChromeProvider() = default;
    [[nodiscard]] virtual EditorChromeSpec& editorChrome() = 0;
};

class EditorArea final : public QFrame {
    Q_OBJECT

  public:
    explicit EditorArea(const EditorRegistry& registry, std::string_view initialEditorId = {},
                        QString areaId = {}, QWidget* parent = nullptr);

    [[nodiscard]] static QWidget* buildCanvasChrome(const EditorCanvasChromeSpec& spec,
                                                    QWidget* parent);
    [[nodiscard]] static QWidget* buildSplitChrome(QWidget* left, QWidget* right, int split,
                                                   QWidget* parent);
    [[nodiscard]] static QWidget* buildChromeRow(EditorChromeRowSpec& spec, QWidget* parent,
                                                 bool footer = false);

    [[nodiscard]] const QString& areaId() const noexcept;
    [[nodiscard]] static bool isValidAreaId(QStringView areaId);
    [[nodiscard]] std::string editorId() const;
    [[nodiscard]] bool setEditorId(std::string_view editorId);

    void setAreaActive(bool active);
    [[nodiscard]] bool isAreaActive() const noexcept;
    void setSplitEnabled(bool enabled);
    void setCloseEnabled(bool enabled);
    void setMaximizedAppearance(bool maximized);

    // task WIDTH-1: a fixed, content-independent floor (kit::Size::PanelMinWidth by this panel's
    // own header+footer height) -- overridden so a selection change inside the hosted editor can
    // never nudge what a QSplitter reads as this panel's minimum, regardless of the hosted
    // editor's own hints. See editor_area.cpp for the full reasoning.
    [[nodiscard]] QSize minimumSizeHint() const override;

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

    const EditorRegistry& editorRegistry_;
    QString areaId_;
    kit::KDropdown* editorPicker_ = nullptr;
    QWidget* editorWidget_ = nullptr;
    // task WIDTH-1: the widget actually parented into contentLayout_ -- editorWidget_ itself for
    // every canvas editor (node graph, timeline, viewer; they already scale their own content down
    // to whatever room they get), or a QScrollArea wrapping editorWidget_ for Properties, the one
    // editor that is a form rather than a canvas. Tracked separately from editorWidget_ so the

    // editor widget regardless of whether it is wrapped.
    QWidget* editorHost_ = nullptr;
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
    QWidget* headerLeft_ = nullptr;
    QWidget* headerRight_ = nullptr;
    QHBoxLayout* headerCellsLayout_ = nullptr;
    // Self-containment (task C1, item C5, corrected by FORMAL AMENDMENT 1): OPTIONAL. Non-null

    // footer widget (ViewerEditor is the only one today); nullptr for every other editor, which
    // gets no footer row at all -- its content extends to the panel's own bottom border instead.
    // Owned by EditorArea from the moment it is taken (reparented here in rebuildEditor()),
    // rebuilt every time the editor changes.
    QWidget* footer_ = nullptr;
    // Task NODES-1: the header's own OPTIONAL extra, on the same terms as footer_ above -- non-null

    // widget (the node editor is the only one today). Lives in headerLayout_, between the panel
    // switcher and the stretch; rebuilt every time the editor changes.
    QWidget* headerMenus_ = nullptr;
    // The rounded frame, painted last and kept above every child (kit::KPanelFrame).
    kit::KPanelFrame* frameOverlay_ = nullptr;
    QMenu* contextMenu_ = nullptr;
    QToolButton* maximizeButton_ = nullptr;
    bool active_ = false;
};

} // namespace bloom::ui
