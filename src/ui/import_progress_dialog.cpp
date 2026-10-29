#include "import_progress_dialog.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/progress_bar.hpp>

#include <QApplication>
#include <QEvent>

#include <algorithm>

namespace bloom::ui {
namespace {

// A sequence named by its first member, without the frame number: "SC020_SH070_image_0001.png"
// reads "SC020_SH070_image" (the separator before the number goes with it).
QString sequenceName(const QString& file) {
    const auto dot = file.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? file.left(dot) : file;
    auto end = stem.size();
    while (end > 0 && !stem.at(end - 1).isDigit())
        --end;
    auto begin = end;
    while (begin > 0 && stem.at(begin - 1).isDigit())
        --begin;
    if (begin == end)
        return stem;
    QString prefix = stem.left(begin);
    while (!prefix.isEmpty() && QStringLiteral("._- ").contains(prefix.back()))
        prefix.chop(1);
    const QString name = prefix + stem.mid(end);
    return name.isEmpty() ? stem : name;
}

// Holds back pointer, wheel, key and drop input while an import runs and its dialog is not shown
// yet, so the import blocks the window from its first moment without a dialog flashing up.
class InputHold final : public QObject {
  public:
    using QObject::QObject;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::Shortcut:
        case QEvent::ContextMenu:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
            return watched->isWidgetType();
        default:
            return false;
        }
    }
};

} // namespace

ImportProgressDialog::ImportProgressDialog(QWidget& owner)
    : kit::KTaskDialog(tr("Import"), QStringLiteral("importProgressDialog"), owner) {
    heading()->setObjectName(QStringLiteral("importProgressHeading"));
    heading()->setText(tr("Importing"));
    detail()->setObjectName(QStringLiteral("importProgressFile"));
    progressBar()->setObjectName(QStringLiteral("importProgressBar"));
    progressBar()->setAccessibleName(tr("Import progress"));
    counts()->setObjectName(QStringLiteral("importSummaryCounts"));
    note()->setObjectName(QStringLiteral("importSummaryNote"));
    summaryList()->setObjectName(QStringLiteral("importSkippedList"));
    setSummaryItemNames(QStringLiteral("importSkippedFile"), QStringLiteral("importSkippedReason"));
    cancelButton()->setObjectName(QStringLiteral("importCancelButton"));
    closeButton()->setObjectName(QStringLiteral("importCloseButton"));
}

void ImportProgressDialog::showProgress(const AssetController::ImportProgressState& state) {
    if (summarizing())
        return;
    QString heading;
    QString detail;
    if (state.finding) {
        // The work total is not known yet: say what is being searched and how much was found.
        if (state.findingFrames) {
            heading = tr("Finding frames…");
            detail = tr("%1 · %2 found").arg(sequenceName(state.current)).arg(state.found);
        } else {
            heading = tr("Finding files…");
            detail = state.found > 0
                         ? tr("Looking in %1 · %2 found").arg(state.current).arg(state.found)
                         : tr("Looking in %1").arg(state.current);
        }
        progressBar()->setBusy();
    } else if (!state.current.isEmpty()) {
        const auto item =
            state.frames > 0 ? tr("%1 · sequence").arg(sequenceName(state.current)) : state.current;
        heading = state.total > 1 ? tr("Importing %1 of %2: %3")
                                        .arg(std::min(state.completed + 1, state.total))
                                        .arg(state.total)
                                        .arg(item)
                                  : tr("Importing %1").arg(item);
        detail = state.frames > 0 ? tr("Reading frame %1 of %2")
                                        .arg(std::min(state.framesRead + 1, state.frames))
                                        .arg(state.frames)
                                  : tr("Reading file");
        const std::pair reading{state.completed, state.current};
        if (reading != readingItem_ || !readingSince_.isValid()) {
            readingItem_ = reading;
            readingSince_.start();
        }
        const bool unmeasured =
            state.frames == 0 &&
            (state.workDone == 0 || readingSince_.elapsed() >= kItemBusyAfter.count());
        if (unmeasured)
            progressBar()->setBusy();
        else
            progressBar()->setProgress(state.workDone, state.workTotal);
    } else {
        progressBar()->setProgress(state.workDone, state.workTotal);
        return;
    }
    kit::KTaskDialog::showProgress(heading, detail);
    if (state.stopping)
        showStopping(tr("Cancelling…"));
}

void ImportProgressDialog::showSummary(const AssetController::ImportReport& report) {
    const bool refused = !report.failure.isEmpty();
    QString heading;
    if (refused || report.imported == 0)
        heading =
            report.cancelled && !refused ? tr("Import cancelled") : tr("Nothing was imported");
    else
        heading = report.cancelled ? tr("Import cancelled") : tr("Import finished");
    QStringList parts;
    parts << tr("Imported %1").arg(refused ? 0 : report.imported);
    if (!report.skipped.empty())
        parts << tr("Skipped %1").arg(report.skipped.size());
    if (report.cancelled && report.notImported > 0)
        parts << tr("Not imported %1").arg(report.notImported);
    QString note;
    if (refused)
        note = report.failure;
    else if (report.cancelled && report.imported > 0)
        note = tr("What was imported before you cancelled was kept. One Undo removes it.");
    else if (report.cancelled)
        note = tr("You cancelled before anything was imported.");
    // What happened next, such as the layers a Timeline drop added.
    if (!report.followUp.isEmpty())
        note = note.isEmpty() ? report.followUp + QLatin1Char('.')
                              : note + QLatin1Char(' ') + report.followUp + QLatin1Char('.');
    std::vector<SummaryItem> items;
    items.reserve(report.skipped.size());
    for (const auto& skipped : report.skipped)
        items.push_back({skipped.file, skipped.reason});
    kit::KTaskDialog::showSummary(heading, parts.join(QStringLiteral(" · ")), note, items);
}

ImportProgressPresenter::ImportProgressPresenter(AssetController& controller, QWidget& window)
    : QObject(&window), controller_(&controller), window_(window),
      timing_(kShowDelay, kRefreshInterval) {
    connect(&timing_, &kit::KTaskTiming::showDue, this, &ImportProgressPresenter::showDialog);
    connect(&timing_, &kit::KTaskTiming::refreshDue, this, &ImportProgressPresenter::refresh);
    connect(&controller, &AssetController::importStarted, this, &ImportProgressPresenter::started);
    connect(&controller, &AssetController::importFinished, this,
            &ImportProgressPresenter::finished);
}

ImportProgressPresenter::~ImportProgressPresenter() { holdInput(false); }

void ImportProgressPresenter::holdInput(const bool held) {
    if (held && !inputHold_) {
        inputHold_ = std::make_unique<InputHold>();
        qApp->installEventFilter(inputHold_.get());
    } else if (!held && inputHold_) {
        qApp->removeEventFilter(inputHold_.get());
        inputHold_.reset();
    }
}

ImportProgressDialog* ImportProgressPresenter::ensureDialog() {
    if (dialog_ && dialog_->summarizing()) {
        dialog_->dismiss();
        dialog_.clear();
    }
    if (!dialog_) {
        dialog_ = new ImportProgressDialog(window_);
        dialog_->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog_, &ImportProgressDialog::stopRequested, this, [this] {
            if (controller_)
                controller_->stopImport();
        });
    }
    return dialog_;
}

void ImportProgressPresenter::started() {
    ensureDialog();
    holdInput(true);
    timing_.begin();
}

void ImportProgressPresenter::showDialog() {
    if (!controller_ || !controller_->importing()) {
        timing_.finish();
        return;
    }
    holdInput(false);
    refresh();
    dialog_->show();
    dialog_->raise();
}

void ImportProgressPresenter::refresh() {
    if (controller_ && dialog_ && controller_->importing())
        dialog_->showProgress(controller_->importProgress());
}

void ImportProgressPresenter::finished(const AssetController::ImportReport& report) {
    timing_.finish();
    holdInput(false);
    const bool summary = !report.discarded && (report.cancelled || !report.failure.isEmpty() ||
                                               !report.skipped.empty() || report.imported == 0);
    if (!summary) {
        if (dialog_)
            dialog_->dismiss();
        if (!report.discarded) {
            auto text = report.imported == 1 ? tr("Imported 1 asset")
                                             : tr("Imported %1 assets").arg(report.imported);
            if (!report.followUp.isEmpty())
                text += QStringLiteral(" · ") + report.followUp;
            emit notice(text);
        }
        return;
    }
    auto* dialog = dialog_ ? dialog_.data() : ensureDialog();
    dialog->showSummary(report);
    dialog->show();
    dialog->raise();
}

} // namespace bloom::ui
