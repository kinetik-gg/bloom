#pragma once

// Details…, from a job's ⋯ menu: what the job did, in plain words. The result sentence, the
// plain lines about the check of each file, a Show in folder per saved output, and the run's log,
// with Copy details for a bug report (the host's raw wording and codes go only there). It reads
// the snapshot and the adapter's bounded log, follows the job while it stays open, and owns
// nothing of the queue.

#include <bloom/document/ids.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <QString>
#include <QStringList>

#include <cstdint>

class QVBoxLayout;

namespace bloom::ui {
class ExportQueueAdapter;
namespace kit {
class KTextArea;
}

namespace exportpanel {

class JobDetailsDialog final : public kit::KDialog {
    Q_OBJECT

  public:
    JobDetailsDialog(ExportQueueAdapter& adapter, document::ExportJobId job, QWidget& owner);

    // Whether a job has anything to report: it has run, or has finished.
    [[nodiscard]] static bool hasResults(const host::ExportJobView& job);

    // Test seams.
    [[nodiscard]] QString summaryText() const;
    [[nodiscard]] QStringList lineTexts() const { return lineTexts_; }
    [[nodiscard]] QString logText() const;
    // What Copy details puts on the clipboard.
    [[nodiscard]] QString detailsText() const;

  private:
    void refresh();
    void refreshLog(const host::ExportJobView& job);

    ExportQueueAdapter& adapter_;
    document::ExportJobId job_;
    kit::KLabel* summary_ = nullptr;
    kit::KLabel* reason_ = nullptr;
    kit::KLabel* changed_ = nullptr;
    QVBoxLayout* lines_ = nullptr;
    kit::KTextArea* log_ = nullptr;
    QString signature_;
    QStringList lineTexts_;
    QString rawDetails_;
    std::uint64_t logSequence_ = 0;
    std::uint64_t loggedRun_ = 0;
};

} // namespace exportpanel
} // namespace bloom::ui
