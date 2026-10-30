#pragma once

// One job in the Export panel's list. The header is one Assets-style row: caret, label colour,
// "Include in Start All" switch, name, composition, range, status, progress and the one job button
// that fits the job's state. The body (shown when the row is open) holds the Render group, one
// OutputRow per output, Add output and, once the job has run, its Result and Log.
//
// The row holds no queue state. It is filled from a snapshot view and issues every edit and
// session command through the application's ExportQueueAdapter, so each gesture is one command
// (one undo entry for project truth) and the host's reasons stay visible as labels.

#include "export_colour_choices.hpp"

#include <bloom/document/ids.hpp>
#include <bloom/host/export_queue.hpp>

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QMenu;

namespace bloom::ui {
struct ExportEditResult;
class CompositionSession;
class ExportQueueAdapter;
namespace kit {
class KButton;
class KProgressBar;
class KRow;
class KSection;
} // namespace kit

namespace exportpanel {

class JobResults;
class OutputRow;

// The one verb the job button shows. The others stay in the row's overflow menu.
enum class JobVerb : std::uint8_t { Start, Pause, Resume, Stop, Retry };
[[nodiscard]] JobVerb contextualVerb(const host::ExportJobView& job);
[[nodiscard]] QString verbText(JobVerb verb);

class JobRow final : public QWidget {
    Q_OBJECT

  public:
    JobRow(CompositionSession& session, ExportQueueAdapter& adapter, document::ExportJobId job,
           QWidget* parent);
    ~JobRow() override;

    [[nodiscard]] document::ExportJobId job() const noexcept { return job_; }
    void update(const host::ExportJobView& job,
                const host::ExportPresetCatalogueSnapshot* catalogue,
                const ExportColourChoices& colours);
    // Where this row sits in the list, so Move up and Move down know when they cannot.
    void setPosition(int index, int count);
    void setCurrent(bool current);

    [[nodiscard]] bool expanded() const noexcept { return expanded_; }
    void setExpanded(bool expanded);
    [[nodiscard]] OutputRow* outputRow(document::ExportOutputId output) const;
    [[nodiscard]] std::vector<OutputRow*> outputRows() const;
    [[nodiscard]] JobResults* results() const noexcept { return results_; }

    // Opens the name for editing in place (double click or F2).
    void startRename();
    // Add output, in the format of `presetId`: one undoable edit. The Add output menu calls this.
    void addOutputWithFormat(const QString& presetId);
    [[nodiscard]] std::unique_ptr<QMenu> buildMenu();

    // Test seams: what the header and the body show.
    [[nodiscard]] QString nameText() const;
    [[nodiscard]] QString compositionText() const;
    [[nodiscard]] QString rangeText() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString progressText() const;
    [[nodiscard]] QString reasonsText() const;
    [[nodiscard]] bool progressShown() const;
    [[nodiscard]] bool twoLineHeader() const noexcept { return narrow_; }
    [[nodiscard]] kit::KButton* jobButton() const noexcept { return button_; }
    [[nodiscard]] kit::KRow* header() const noexcept { return header_; }
    [[nodiscard]] kit::KSection* renderSection() const noexcept { return render_; }

  Q_SIGNALS:
    void expandedChanged(qulonglong job, bool expanded);
    void activated(qulonglong job);
    // Move the job one place up (-1) or down (+1) in the list.
    void moveRequested(qulonglong job, int direction);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    struct Fields;
    void build();
    void buildHeader();
    void buildBody();
    void applyLayoutMode(bool narrow);
    void applyColumns();
    void showMenu(QPoint globalPosition);
    void runVerb();
    void commitRange();
    void endRename();
    void addOutputMenu();
    [[nodiscard]] const host::ExportJobView* findJob() const;
    void report(const ExportEditResult& result);
    [[nodiscard]] int singleLineWidth() const;

    CompositionSession& session_;
    ExportQueueAdapter& adapter_;
    document::ExportJobId job_;
    bool expanded_ = false;
    bool narrow_ = false;
    bool refreshing_ = false;
    bool renaming_ = false;
    bool progressWanted_ = false;
    bool current_ = false;
    int index_ = 0;
    int count_ = 1;
    JobVerb verb_ = JobVerb::Start;
    kit::KRow* header_ = nullptr;
    kit::KButton* button_ = nullptr;
    kit::KSection* render_ = nullptr;
    JobResults* results_ = nullptr;
    const host::ExportPresetCatalogueSnapshot* catalogue_ = nullptr;
    ExportColourChoices colours_;
    std::unique_ptr<Fields> f_;
};

} // namespace exportpanel
} // namespace bloom::ui
