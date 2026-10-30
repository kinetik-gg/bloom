#pragma once

// The Export panel: the ADR 0033 render queue in the After Effects Render Queue order, built from
// Bloom's own components. A header with Start All, Stop All and Remove finished; a current-render
// strip while something runs; the job list (jobs with their outputs as child rows) over a detail
// pane for the selected job or output.
//
// The panel is a replaceable editor and holds no queue state. It reads the snapshot the
// application's ExportQueueAdapter publishes, issues commands through the adapter (project-truth
// edits are undoable commands; session actions go to the service) and never pumps. Every command
// reads its typed availability from the snapshot and shows the reason as a tooltip or an inline
// field reason: nothing is disabled silently.

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>

class QScrollArea;
class QSplitter;
class QStackedWidget;

namespace bloom::ui {

class CompositionSession;
class ExportQueueAdapter;
class EditorRegistry;
namespace exportpanel {
class ExportDetailPane;
class ExportJobTree;
class ExportRunStrip;
} // namespace exportpanel
namespace kit {
class KButton;
class KLabel;
} // namespace kit

class ExportEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    // `adapter` may be null (a window built without an export queue): the panel then says so and
    // offers nothing.
    ExportEditor(CompositionSession& session, ExportQueueAdapter* adapter,
                 QWidget* parent = nullptr);
    ~ExportEditor() override;

    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }

    void selectJob(document::ExportJobId job);
    [[nodiscard]] std::optional<document::ExportJobId> selectedJob() const;
    [[nodiscard]] std::optional<document::ExportOutputId> selectedOutput() const;

  private:
    void rebuild();
    void sync();
    void updateSelection();
    void updateHeader();
    void showContextMenu(QPoint position);
    void addActiveComposition();
    void startAll();

    CompositionSession& session_;
    ExportQueueAdapter* adapter_ = nullptr;
    EditorChromeSpec chrome_;
    kit::KButton* startAll_ = nullptr;
    kit::KButton* stopAll_ = nullptr;
    kit::KButton* removeFinished_ = nullptr;
    exportpanel::ExportRunStrip* strip_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    exportpanel::ExportJobTree* tree_ = nullptr;
    QWidget* emptyPage_ = nullptr;
    kit::KLabel* emptyText_ = nullptr;
    kit::KButton* emptyAdd_ = nullptr;
    kit::KLabel* platformNote_ = nullptr;
    QScrollArea* detailScroll_ = nullptr;
    QString rowSignature_;
    exportpanel::ExportDetailPane* detail_ = nullptr;
    std::uint64_t shownRevision_ = ~std::uint64_t{0};
    bool syncing_ = false;
    bool splitterSized_ = false;
};

[[nodiscard]] bool registerExportEditor(EditorRegistry& registry, CompositionSession& session,
                                        ExportQueueAdapter* adapter);

} // namespace bloom::ui
