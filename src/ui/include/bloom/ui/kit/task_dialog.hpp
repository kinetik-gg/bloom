#pragma once

#include <bloom/ui/kit/dialog.hpp>

#include <QTimer>

#include <chrono>
#include <vector>

class QVBoxLayout;

namespace bloom::ui::kit {

class KButton;
class KProgressBar;

// The progress-then-summary modal for long work (generalised from the import dialog, ADR 0029).
// While the work runs it shows an elided heading, an elided muted detail, a KProgressBar and a
// Secondary Cancel; Cancel, Escape and the window's close button only ask the work to stop (the
// dialog stays, reading "Cancelling…", until the work answers). The summary replaces those rows
// with the heading, a counts line, an optional note and a list of items with their reasons, and
// one Primary Close that Return and Escape also take. The work itself never runs here; the owner
// feeds the dialog its latest report.
class KTaskDialog : public KDialog {
    Q_OBJECT
  public:
    struct SummaryItem final {
        QString name;
        QString reason;
    };

    KTaskDialog(const QString& title, const QString& objectName, QWidget& owner,
                Qt::WindowModality modality = Qt::ApplicationModal);

    // Progress mode. The bar is set separately: progressBar()->setBusy() / setProgress().
    void showProgress(const QString& heading, const QString& detail);
    // Disables Cancel and says the stop is under way.
    void showStopping(const QString& heading);
    void showSummary(const QString& heading, const QString& counts, const QString& note,
                     const std::vector<SummaryItem>& items);
    [[nodiscard]] bool summarizing() const noexcept { return summarizing_; }
    // Closes whatever the dialog shows; the artist's own close only asks the work to stop.
    void dismiss();

    [[nodiscard]] QString headingText() const;
    [[nodiscard]] QString detailText() const;
    [[nodiscard]] KLabel* heading() const noexcept { return heading_; }
    [[nodiscard]] KLabel* detail() const noexcept { return detail_; }
    [[nodiscard]] KLabel* counts() const noexcept { return counts_; }
    [[nodiscard]] KLabel* note() const noexcept { return note_; }
    [[nodiscard]] QWidget* summaryList() const noexcept { return list_; }
    [[nodiscard]] KProgressBar* progressBar() const noexcept { return bar_; }
    [[nodiscard]] KButton* cancelButton() const noexcept { return cancel_; }
    [[nodiscard]] KButton* closeButton() const noexcept { return close_; }
    // Object names of the summary rows' name and reason labels.
    void setSummaryItemNames(const QString& name, const QString& reason);

  Q_SIGNALS:
    void stopRequested();

  public Q_SLOTS:
    void reject() override;

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void requestStop();

    KLabel* heading_ = nullptr;
    KLabel* detail_ = nullptr;
    KProgressBar* bar_ = nullptr;
    KLabel* counts_ = nullptr;
    KLabel* note_ = nullptr;
    QWidget* list_ = nullptr;
    QVBoxLayout* listRows_ = nullptr;
    KButton* cancel_ = nullptr;
    KButton* close_ = nullptr;
    QString detailFull_;
    QString itemName_ = QStringLiteral("kTaskSummaryItem");
    QString itemReason_ = QStringLiteral("kTaskSummaryReason");
    bool summarizing_ = false;
    bool stopping_ = false;
    bool dismissing_ = false;
};

// The task dialog's timing: nothing for `delay` (so quick work never flickers a dialog), then
// `showDue` once and `refreshDue` every `interval` until finish().
class KTaskTiming final : public QObject {
    Q_OBJECT
  public:
    KTaskTiming(std::chrono::milliseconds delay, std::chrono::milliseconds interval,
                QObject* parent = nullptr);
    void begin();
    void finish();
    [[nodiscard]] bool waiting() const { return delay_.isActive(); }

  Q_SIGNALS:
    void showDue();
    void refreshDue();

  private:
    QTimer delay_;
    QTimer refresh_;
};

} // namespace bloom::ui::kit
