#pragma once

#include <bloom/ui/native_surface_retirement.hpp>

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
        bool expanding = false;
    };
    QString objectName;
    QString overflowButtonName;
    QString overflowMenuName;
    std::vector<Entry> entries;
    QPointer<QWidget> host;
    QWidget* owner = nullptr;
    bool trailing = false;
    void addWidget(QWidget* control);
    void addExpandingWidget(QWidget* control);
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
    // A panel may declare a fixed leading control strip for its content. Canvas editors usually
    // materialize this inside their own body; when a provider gives EditorArea a detached widget,
    // the area hosts it beside the editor content using these token/name hints.
    QWidget* leading = nullptr;
    int leadingWidth = 0;
    QString leadingName;
    QWidget* headerCanvas = nullptr;
    // Optional split canvas footer, built through the same chrome helpers as the header.
    QWidget* footerCanvas = nullptr;
    bool maximizeInLeadingHeader = false;
    std::function<int()> splitPosition;
    // task TL-FIX2: EditorArea fixes headerLeft_'s width to splitPosition() once, at rebuild time
    // (see EditorArea::rebuildEditor()). A provider whose split moves later -- the timeline's
    // draggable layer-table/lanes divider -- calls this (populated by EditorArea alongside
    // headerCanvas) to re-apply the current splitPosition() without a full rebuild.
    std::function<void()> refreshSplit;
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
    // Non-empty id: always returns true. When a live native-surface editor is being replaced the
    // actual rebuild is deferred (documented pending result); poll isEditorChangePending() and see
    // lastNativeSurfaceDiagnostic() on refusal. A CPU-only editor applies synchronously as before.
    [[nodiscard]] bool setEditorId(std::string_view editorId);

    // True while an editor replacement is waiting for its outgoing native surface to retire. During
    // this window editorId(), the hosted widget, its parent, and the footer are all unchanged.
    [[nodiscard]] bool isEditorChangePending() const noexcept;

    // The optional native-surface lifecycle interface of the currently hosted editor, or nullptr
    // for CPU-only editors. WorkspaceHost uses this to preflight a whole subtree.
    [[nodiscard]] EditorNativeSurface* nativeSurface() const noexcept;

    // Honest diagnostic from the last refused replacement (empty when none).
    [[nodiscard]] const std::string& lastNativeSurfaceDiagnostic() const noexcept;

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
    // Gated picker entry point: CPU-only editors fall straight through to rebuildEditor(); a live
    // native surface is retired first and the rebuild is deferred to the completion.
    void requestEditorChange(int editorIndex);
    void applyEditorChange(int editorIndex);
    void revertPickerToAppliedIndex();
    void onEditorChangeRetired(const NativeSurfaceRetirementGate::Result& result);
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
    // Retire-before-replace gate for the outgoing editor's native surface (CPU-only: inert).
    NativeSurfaceRetirementGate surfaceRetirementGate_;
    // The editor index whose materialization currently matches editorWidget_/footer_/headerMenus_.
    // -1 until the first successful rebuild. The picker is reverted to this while a retirement is
    // pending so no selection-truth change is visible before the mutation actually happens.
    int appliedEditorIndex_ = -1;
    std::string lastNativeSurfaceDiagnostic_;
};

} // namespace bloom::ui
