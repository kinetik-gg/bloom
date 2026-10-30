#pragma once

// What a job did, in plain words: one summary sentence, a Show in folder button per saved output,
// at most a few plain lines about the check of the file, and a collapsed Log with a Copy details
// button for bug reports. Shown by a job row only once the job has run.
//
// The widget reads a snapshot view and the adapter's bounded log; it owns nothing of the queue.

#include <bloom/document/ids.hpp>
#include <bloom/host/export_queue.hpp>

#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::ui {
class ExportQueueAdapter;
namespace kit {
class KSection;
}

namespace exportpanel {

class JobResults final : public QWidget {
    Q_OBJECT

  public:
    JobResults(ExportQueueAdapter& adapter, document::ExportJobId job, QWidget* parent);
    ~JobResults() override;

    void update(const host::ExportJobView& job,
                const host::ExportPresetCatalogueSnapshot* catalogue);
    // Whether the job has anything to report: it has run, or has finished.
    [[nodiscard]] static bool hasResults(const host::ExportJobView& job);

    // Test seams.
    [[nodiscard]] QString summaryText() const;
    [[nodiscard]] QStringList lineTexts() const;
    [[nodiscard]] QString logText() const;
    [[nodiscard]] kit::KSection* logSection() const noexcept { return log_; }

  private:
    struct Fields;
    void refreshLog(const host::ExportJobView& job);

    ExportQueueAdapter& adapter_;
    document::ExportJobId job_;
    kit::KSection* result_ = nullptr;
    kit::KSection* log_ = nullptr;
    std::uint64_t logSequence_ = 0;
    std::uint64_t loggedRun_ = 0;
    std::unique_ptr<Fields> f_;
};

} // namespace exportpanel
} // namespace bloom::ui
