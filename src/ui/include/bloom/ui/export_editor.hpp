#pragma once

// The Export panel: the ADR 0033 render queue as After Effects' Render Queue, built from Bloom's
// own components. Top to bottom:
//
// - the panel header: the editor picker, Add composition (Secondary); on the right Start All (the
//   one Primary while a job is ready), Stop All while something renders, and the maximize button;
// - the Current Render strip, folded to one line ("Rendering SC020 · frame 404 of 2156 · 0:28
//   elapsed · about 2 min left", or "Idle"), opened to add a progress bar;
// - the render list: the Assets row system with real column headers (Render, #, Composition,
//   Status, Started, Render Time). A job opens to its "Render Settings" line and one "Output
//   Module" line per output, whose Accent links open the Render Settings and Output Module dialogs
//   and the file chooser; each ▾ is a short menu of quick picks.
//
// The panel is a replaceable editor and holds no queue state. It reads the snapshot the
// application's ExportQueueAdapter publishes, issues commands through the adapter (project-truth
// edits are undoable commands, one per gesture or dialog OK; session actions go to the service)
// and never pumps. A command that cannot run is disabled with the host's reason as its tooltip.

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QPoint>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QDragEnterEvent;
class QDragLeaveEvent;
class QMenu;
class QMimeData;
class QDragMoveEvent;
class QDropEvent;
class QStackedWidget;
class QTreeWidgetItem;

namespace bloom::ui {

struct ExportEditResult;
class CompositionSession;
class ExportQueueAdapter;
class EditorRegistry;
namespace exportpanel {
class CheckCell;
class JobDetailsDialog;
class OutputModuleDialog;
class OutputModuleLine;
class RenderList;
class RenderSettingsDialog;
class RenderSettingsLine;
class StatusCell;
class SummaryLine;
struct ExportColourChoices;
} // namespace exportpanel
namespace kit {
class KButton;
class KIconButton;
class KLabel;
class KProgressBar;
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

    // Makes `job` the current row: selects it, opens it and scrolls it into view.
    void selectJob(document::ExportJobId job);
    [[nodiscard]] std::optional<document::ExportJobId> selectedJob() const;
    void setJobExpanded(document::ExportJobId job, bool expanded);

    [[nodiscard]] exportpanel::RenderList* renderList() const noexcept { return list_; }
    // The job ids in list order.
    [[nodiscard]] std::vector<document::ExportJobId> jobOrder() const;
    // What a job's row shows (nullptr when the job is not listed).
    [[nodiscard]] QTreeWidgetItem* jobItem(document::ExportJobId job) const;
    [[nodiscard]] exportpanel::CheckCell* renderCheck(document::ExportJobId job) const;
    [[nodiscard]] exportpanel::StatusCell* statusCell(document::ExportJobId job) const;
    [[nodiscard]] exportpanel::RenderSettingsLine* renderSettingsLine(document::ExportJobId job) const;
    [[nodiscard]] exportpanel::OutputModuleLine* outputLine(document::ExportJobId job,
                                                            document::ExportOutputId output) const;
    // The summary a line's disclosure shows (nullptr for a row that has none).
    [[nodiscard]] exportpanel::SummaryLine* summaryLine(QTreeWidgetItem* line) const;
    // The row's ⋯ menu, as the button opens it.
    [[nodiscard]] std::unique_ptr<QMenu> buildJobMenu(document::ExportJobId job);

    // The dialogs the links open. Each is shown (window-modal) and returned; OK writes one edit.
    exportpanel::RenderSettingsDialog* openRenderSettings(document::ExportJobId job);
    exportpanel::OutputModuleDialog* openOutputModule(document::ExportJobId job,
                                                      document::ExportOutputId output);
    exportpanel::JobDetailsDialog* openDetails(document::ExportJobId job);

    // Start All and a job's own Start: when a destination exists under "Ask first", they ask once
    // (kit::confirm) before anything starts.
    void startAll();
    void startJob(document::ExportJobId job);

    // The Current Render strip.
    [[nodiscard]] QString currentRenderText() const;
    [[nodiscard]] bool currentRenderExpanded() const noexcept { return stripExpanded_; }
    void setCurrentRenderExpanded(bool expanded);

    // Where a drop at `position` (this widget's coordinates) would insert: among the jobs, from 0 to
    // the job count. Whether the drop indicator shows, and the y of its line in the list.
    [[nodiscard]] std::size_t dropIndexAt(QPoint position) const;
    [[nodiscard]] bool dropIndicatorShown() const;
    [[nodiscard]] int dropIndicatorY() const;

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    void buildStrip();
    void buildEmptyPage();
    void sync();
    void rebuildItems();
    void updateItems();
    void updateHeader();
    void updateStrip();
    void refitLines();
    void connectJob(QTreeWidgetItem* item);
    void addActiveComposition();
    void showJobMenu(document::ExportJobId job, QPoint globalPosition);
    void report(const ExportEditResult& result);
    void quickRange(document::ExportJobId job, int kind);
    void chooseDestination(document::ExportJobId job, document::ExportOutputId output);
    void useDefaultName(document::ExportJobId job, document::ExportOutputId output);
    void useFolder(document::ExportJobId job, document::ExportOutputId output, const QString& folder);
    // Asks once about every output of `jobs` that would replace a file; `then` runs after Replace.
    void confirmReplacements(const std::vector<document::ExportJobId>& jobs,
                             const std::function<void()>& then);
    [[nodiscard]] bool acceptsDrag(const QMimeData& mime) const;
    // The compositions an Assets drag carries, in selection order; anything else is ignored.
    [[nodiscard]] std::vector<document::CompositionId> draggedCompositions(const QMimeData& mime) const;
    [[nodiscard]] std::optional<document::ExportJobId> draggedJob(const QMimeData& mime) const;
    void showDropIndicator(QPoint position);
    void hideDropIndicator();

    CompositionSession& session_;
    ExportQueueAdapter* adapter_ = nullptr;
    EditorChromeSpec chrome_;
    kit::KButton* startAll_ = nullptr;
    kit::KButton* stopAll_ = nullptr;
    kit::KButton* addComposition_ = nullptr;
    kit::KSurface* strip_ = nullptr;
    kit::KIconButton* stripToggle_ = nullptr;
    kit::KLabel* stripText_ = nullptr;
    kit::KProgressBar* stripBar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    exportpanel::RenderList* list_ = nullptr;
    kit::KSurface* emptyPage_ = nullptr;
    kit::KSurface* dropLine_ = nullptr;
    kit::KLabel* emptyText_ = nullptr;
    kit::KLabel* emptyHelp_ = nullptr;
    kit::KButton* emptyAdd_ = nullptr;
    std::unique_ptr<exportpanel::ExportColourChoices> colours_;
    // Session state of the list (never saved): which jobs and lines are open.
    QSet<qulonglong> openJobs_;
    QSet<QString> openLines_;
    std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> shownStructure_;
    std::optional<document::ExportJobId> pendingSelection_;
    std::uint64_t shownRevision_ = ~std::uint64_t{0};
    bool coloursStale_ = true;
    bool syncing_ = false;
    bool emptyDropShown_ = false;
    bool stripExpanded_ = false;
};

[[nodiscard]] bool registerExportEditor(EditorRegistry& registry, CompositionSession& session,
                                        ExportQueueAdapter* adapter);

} // namespace bloom::ui
