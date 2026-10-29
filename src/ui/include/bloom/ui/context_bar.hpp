#pragma once

#include <bloom/ui/tool_controller.hpp>

#include <QPointer>
#include <QWidget>

#include <map>
#include <vector>

class QAction;
class QKeyEvent;
class QMenu;

namespace bloom::ui::kit {
class KIconButton;
class KIconToggle;
class KLabel;
class KDropdown;
} // namespace bloom::ui::kit

namespace bloom::ui {

class CompositionSession;
class CompositionPreviewController;
class LayerAlignmentActions;
class ViewerEditor;

// ADR 0031: the fixed contextual bar directly under the menu bar. Left, the app-wide tools; middle,
// the active tool's options; right, Align and Distribute for the selected layers. It reads session
// and document state and issues the same commands every other surface does; it owns no project
// truth. Its buttons never take keyboard focus, and a field in it hands focus back to the Viewer
// that last had it once it commits. A narrow window collapses its groups, right first, into one
// overflow menu.
class ContextBar final : public QWidget {
    Q_OBJECT

  public:
    ContextBar(CompositionSession& session, CompositionPreviewController* preview,
               QWidget* parent = nullptr);
    ~ContextBar() override;

    // The Viewer the bar's anchor handle modes act on and focus returns to: the active, else the
    // last active, visible Viewer (MainWindow tracks it). Null when there is none.
    void setViewer(ViewerEditor* viewer);
    [[nodiscard]] ViewerEditor* viewer() const noexcept { return viewer_; }
    [[nodiscard]] LayerAlignmentActions& alignment() const noexcept { return *alignment_; }
    // Test seams.
    [[nodiscard]] kit::KIconToggle* toolButtonForTest(ViewerTool tool) const;
    [[nodiscard]] QMenu* shapeFlyoutForTest() const noexcept { return shapeFlyout_; }

  signals:
    // A status line for the window: layers an alignment skipped, and why.
    void notice(const QString& message);

  protected:
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void buildTools();
    QWidget* buildAlignment(QWidget* parent, bool distribute);
    QWidget* buildAlignTarget(QWidget* parent);
    void refreshTools();
    void refreshOptions();
    void refreshAlignTarget();
    void returnFocusToViewer();

    CompositionSession& session_;
    CompositionPreviewController* preview_;
    LayerAlignmentActions* alignment_ = nullptr;
    QPointer<ViewerEditor> viewer_;
    QWidget* row_ = nullptr;
    QWidget* tools_ = nullptr;
    std::map<ViewerTool, kit::KIconToggle*> toolButtons_;
    // The shared Rectangle/Ellipse/Polygon/Star slot and its flyout.
    kit::KIconToggle* shapeSlot_ = nullptr;
    QMenu* shapeFlyout_ = nullptr;
    // One options group per kind of tool; only the active tool's is in the row.
    QWidget* anchorOptions_ = nullptr;
    QWidget* shapeOptions_ = nullptr;
    QWidget* textOptions_ = nullptr;
    // The anchor-mode groups in the row and in the overflow menu, refreshed from the Viewer.
    std::vector<QWidget*> anchorOverflows_;
    kit::KDropdown* alignTarget_ = nullptr;
    QMetaObject::Connection viewerConnection_;
};

} // namespace bloom::ui
