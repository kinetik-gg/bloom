#pragma once

// The Export panel: the ADR 0033 render queue in the After Effects Render Queue order, built from
// Bloom's own components. One scrolling list of job rows: each job is a header (label colour,
// "Include in Start All", name, composition, range, status, progress and the one button that fits
// its state) over its Render group, its outputs (each with its Format, Save to, "If a file is
// there", Quality and After render) and, once it has run, its Result. There is no detail pane, no
// splitter and no separate progress strip. The header offers Add composition, Start All (the one
// Primary, shown only while a job is ready), Stop All while something runs, and Remove finished.
//
// The panel is a replaceable editor and holds no queue state. It reads the snapshot the
// application's ExportQueueAdapter publishes, issues commands through the adapter (project-truth
// edits are undoable commands; session actions go to the service) and never pumps. Every command
// reads its typed availability from the snapshot and shows the reason as a visible label:
// nothing is disabled silently.

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QDragEnterEvent;
class QDragLeaveEvent;
class QMimeData;
class QDragMoveEvent;
class QDropEvent;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace bloom::ui {

class CompositionSession;
class ExportQueueAdapter;
class EditorRegistry;
namespace exportpanel {
class JobRow;
struct ExportColourChoices;
} // namespace exportpanel
namespace kit {
class KButton;
class KLabel;
class KSurface;
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

    // Makes `job` the current row: opens it and scrolls it into view.
    void selectJob(document::ExportJobId job);
    [[nodiscard]] std::optional<document::ExportJobId> selectedJob() const;
    // The job rows in list order, and one by id (nullptr when absent).
    [[nodiscard]] std::vector<exportpanel::JobRow*> jobRows() const;
    [[nodiscard]] exportpanel::JobRow* jobRow(document::ExportJobId job) const;
    // Where a drop at `position` (this widget's coordinates) would insert: among the jobs, from 0 to
    // the job count. Whether the drop indicator shows, and the y of its line in the list.
    [[nodiscard]] std::size_t dropIndexAt(QPoint position) const;
    [[nodiscard]] bool dropIndicatorShown() const;
    [[nodiscard]] int dropIndicatorY() const;
    // What the header says about the last Start All ("2 started, 1 waiting for your OK").
    [[nodiscard]] QString startNoteText() const;

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    void rebuild();
    void sync();
    void updateHeader();
    void addActiveComposition();
    void startAll();
    void moveJob(qulonglong job, int direction);
    // The compositions an Assets drag carries, in selection order; anything else is ignored.
    [[nodiscard]] std::vector<document::CompositionId> draggedCompositions(const QMimeData& mime) const;
    void showDropIndicator(QPoint position);
    void hideDropIndicator();

    CompositionSession& session_;
    ExportQueueAdapter* adapter_ = nullptr;
    EditorChromeSpec chrome_;
    kit::KButton* startAll_ = nullptr;
    kit::KButton* stopAll_ = nullptr;
    kit::KButton* removeFinished_ = nullptr;
    kit::KButton* addComposition_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
    kit::KSurface* emptyPage_ = nullptr;
    kit::KSurface* dropLine_ = nullptr;
    kit::KLabel* emptyText_ = nullptr;
    kit::KLabel* emptyHelp_ = nullptr;
    kit::KButton* emptyAdd_ = nullptr;
    kit::KLabel* platformNote_ = nullptr;
    kit::KLabel* startNote_ = nullptr;
    std::vector<exportpanel::JobRow*> jobRows_;
    std::optional<document::ExportJobId> current_;
    std::optional<document::ExportJobId> pendingSelection_;
    std::unique_ptr<exportpanel::ExportColourChoices> colours_;
    bool coloursStale_ = true;
    std::uint64_t shownRevision_ = ~std::uint64_t{0};
    int startedByStartAll_ = -1;
    bool syncing_ = false;
    bool emptyDropShown_ = false;
};

[[nodiscard]] bool registerExportEditor(EditorRegistry& registry, CompositionSession& session,
                                        ExportQueueAdapter* adapter);

} // namespace bloom::ui
