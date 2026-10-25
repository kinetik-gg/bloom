#include "import_progress_dialog.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui {
namespace {

void setInk(QWidget& widget, const kit::Color ink) {
    QPalette palette = widget.palette();
    palette.setColor(QPalette::WindowText, kit::color(ink));
    widget.setPalette(palette);
}

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

ImportProgressDialog::ImportProgressDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("importProgressDialog"));
    setWindowTitle(tr("Import"));
    setMinimumWidth(kit::px(kit::Size::DialogTextWidth));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    heading_ = new kit::KLabel(tr("Importing"), this);
    heading_->setObjectName(QStringLiteral("importProgressHeading"));
    layout->addWidget(heading_);
    detail_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    detail_->setObjectName(QStringLiteral("importProgressFile"));
    setInk(*detail_, kit::Color::Muted);
    layout->addWidget(detail_);
    bar_ = new kit::KProgressBar(this);
    bar_->setObjectName(QStringLiteral("importProgressBar"));
    bar_->setAccessibleName(tr("Import progress"));
    layout->addWidget(bar_);

    counts_ = new kit::KLabel(QString{}, this);
    counts_->setObjectName(QStringLiteral("importSummaryCounts"));
    counts_->hide();
    layout->addWidget(counts_);
    note_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    note_->setObjectName(QStringLiteral("importSummaryNote"));
    note_->setWordWrap(true);
    setInk(*note_, kit::Color::Muted);
    note_->hide();
    layout->addWidget(note_);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("importSkippedList"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMaximumHeight(kit::px(kit::Size::DiagnosticHeight));
    auto* rows = new QWidget(scroll);
    skippedRows_ = new QVBoxLayout(rows);
    skippedRows_->setContentsMargins(0, 0, 0, 0);
    skippedRows_->setSpacing(kit::px(kit::Spacing::S));
    scroll->setWidget(rows);
    skippedPanel_ = scroll;
    skippedPanel_->hide();
    layout->addWidget(skippedPanel_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    cancel_ = new kit::KButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("importCancelButton"));
    close_ = new kit::KButton(tr("Close"), this);
    close_->setObjectName(QStringLiteral("importCloseButton"));
    close_->setVariant(kit::KButton::Variant::Primary);
    close_->hide();
    buttons->addWidget(cancel_);
    buttons->addWidget(close_);
    layout->addLayout(buttons);
    connect(cancel_, &kit::KButton::clicked, this, &ImportProgressDialog::requestStop);
    connect(close_, &kit::KButton::clicked, this, &QDialog::accept);
}

void ImportProgressDialog::showProgress(const AssetController::ImportProgressState& state) {
    if (summarizing_)
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
        bar_->setBusy();
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
            bar_->setBusy();
        else
            bar_->setProgress(state.workDone, state.workTotal);
    } else {
        bar_->setProgress(state.workDone, state.workTotal);
        return;
    }
    if (state.stopping) {
        heading = tr("Cancelling…");
        cancel_->setEnabled(false);
    }
    heading_->setElidedText(heading);
    detailFull_ = detail;
    detail_->setElidedText(detail);
}

void ImportProgressDialog::showSummary(const AssetController::ImportReport& report) {
    summarizing_ = true;
    const bool refused = !report.failure.isEmpty();
    if (refused || report.imported == 0)
        heading_->setElidedText(report.cancelled && !refused ? tr("Import cancelled")
                                                             : tr("Nothing was imported"));
    else
        heading_->setElidedText(report.cancelled ? tr("Import cancelled") : tr("Import finished"));
    QStringList parts;
    parts << tr("Imported %1").arg(refused ? 0 : report.imported);
    if (!report.skipped.empty())
        parts << tr("Skipped %1").arg(report.skipped.size());
    if (report.cancelled && report.notImported > 0)
        parts << tr("Not imported %1").arg(report.notImported);
    counts_->setText(parts.join(QStringLiteral(" · ")));
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
    note_->setText(note);
    note_->setVisible(!note.isEmpty());

    while (auto* item = skippedRows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto& skipped : report.skipped) {
        auto* row = new QWidget(skippedRows_->parentWidget());
        auto* column = new QVBoxLayout(row);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(kit::px(kit::Spacing::XXS));
        auto* name = new kit::KLabel(skipped.file, row);
        name->setObjectName(QStringLiteral("importSkippedFile"));
        auto* reason = new kit::KLabel(skipped.reason, row, kit::TypeRole::UiSmall);
        reason->setObjectName(QStringLiteral("importSkippedReason"));
        reason->setWordWrap(true);
        setInk(*reason, kit::Color::Muted);
        column->addWidget(name);
        column->addWidget(reason);
        skippedRows_->addWidget(row);
    }
    skippedRows_->addStretch();
    skippedPanel_->setVisible(!report.skipped.empty());

    detail_->hide();
    bar_->hide();
    counts_->show();
    cancel_->hide();
    close_->show();
    close_->setFocus();
    adjustSize();
}

QString ImportProgressDialog::headingText() const {
    return heading_->toolTip().isEmpty() ? heading_->text() : heading_->toolTip();
}

QString ImportProgressDialog::detailText() const {
    return summarizing_ ? counts_->text() : detailFull_;
}

void ImportProgressDialog::dismiss() {
    dismissing_ = true;
    close();
}

void ImportProgressDialog::requestStop() {
    if (summarizing_)
        return;
    cancel_->setEnabled(false);
    heading_->setElidedText(tr("Cancelling…"));
    emit stopRequested();
}

void ImportProgressDialog::reject() {
    // QDialog::closeEvent() closes through reject(), so a dismissal passes straight through.
    if (summarizing_ || dismissing_)
        QDialog::reject();
    else
        requestStop();
}

void ImportProgressDialog::keyPressEvent(QKeyEvent* event) {
    // Return and Enter take the summary's one action, Close; while importing they do nothing.
    if (summarizing_ && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ImportProgressDialog::closeEvent(QCloseEvent* event) {
    if (summarizing_ || dismissing_) {
        QDialog::closeEvent(event);
        return;
    }
    event->ignore();
    requestStop();
}

ImportProgressPresenter::ImportProgressPresenter(AssetController& controller, QWidget& window)
    : QObject(&window), controller_(&controller), window_(window) {
    showTimer_.setSingleShot(true);
    showTimer_.setInterval(kShowDelay);
    refreshTimer_.setInterval(kRefreshInterval);
    connect(&showTimer_, &QTimer::timeout, this, &ImportProgressPresenter::showDialog);
    connect(&refreshTimer_, &QTimer::timeout, this, &ImportProgressPresenter::refresh);
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
        dialog_ = new ImportProgressDialog(&window_);
        dialog_->setAttribute(Qt::WA_DeleteOnClose);
        dialog_->setWindowModality(Qt::ApplicationModal);
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
    showTimer_.start();
}

void ImportProgressPresenter::showDialog() {
    if (!controller_ || !controller_->importing())
        return;
    holdInput(false);
    refresh();
    dialog_->show();
    dialog_->raise();
    refreshTimer_.start();
}

void ImportProgressPresenter::refresh() {
    if (controller_ && dialog_ && controller_->importing())
        dialog_->showProgress(controller_->importProgress());
}

void ImportProgressPresenter::finished(const AssetController::ImportReport& report) {
    showTimer_.stop();
    refreshTimer_.stop();
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
