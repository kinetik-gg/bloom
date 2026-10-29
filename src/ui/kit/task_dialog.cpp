#include <bloom/ui/kit/task_dialog.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/progress_bar.hpp>

#include <QCloseEvent>
#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>

namespace bloom::ui::kit {

KTaskDialog::KTaskDialog(const QString& title, const QString& objectName, QWidget& owner,
                         const Qt::WindowModality modality)
    : KDialog(title, objectName, owner, modality) {
    auto* layout = contentLayout();
    heading_ = new KLabel(QString{}, this);
    layout->addWidget(heading_);
    detail_ = new KLabel(QString{}, this, TypeRole::UiSmall);
    detail_->setTone(Color::Muted);
    layout->addWidget(detail_);
    bar_ = new KProgressBar(this);
    layout->addWidget(bar_);

    counts_ = new KLabel(QString{}, this);
    counts_->hide();
    layout->addWidget(counts_);
    note_ = new KLabel(QString{}, this, TypeRole::UiSmall);
    note_->setWordWrap(true);
    note_->setTone(Color::Muted);
    note_->hide();
    layout->addWidget(note_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMaximumHeight(px(Size::DiagnosticHeight));
    auto* rows = new QWidget(scroll);
    listRows_ = new QVBoxLayout(rows);
    listRows_->setContentsMargins(0, 0, 0, 0);
    listRows_->setSpacing(px(Spacing::S));
    scroll->setWidget(rows);
    list_ = scroll;
    list_->hide();
    layout->addWidget(list_);

    cancel_ = addButton(Role::Reject, tr("Cancel"));
    close_ = addButton(Role::Accept, tr("Close"));
    close_->hide();
}

void KTaskDialog::setSummaryItemNames(const QString& name, const QString& reason) {
    itemName_ = name;
    itemReason_ = reason;
}

void KTaskDialog::showProgress(const QString& heading, const QString& detail) {
    if (summarizing_)
        return;
    // Once the stop is asked for, the heading keeps saying so until the summary.
    if (!stopping_)
        heading_->setElidedText(heading);
    detailFull_ = detail;
    detail_->setElidedText(detail);
}

void KTaskDialog::showStopping(const QString& heading) {
    cancel_->setEnabled(false);
    heading_->setElidedText(heading);
}

void KTaskDialog::showSummary(const QString& heading, const QString& counts, const QString& note,
                              const std::vector<SummaryItem>& items) {
    summarizing_ = true;
    heading_->setElidedText(heading);
    counts_->setText(counts);
    note_->setText(note);
    note_->setVisible(!note.isEmpty());
    while (auto* item = listRows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto& entry : items) {
        auto* row = new QWidget(listRows_->parentWidget());
        auto* column = new QVBoxLayout(row);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(px(Spacing::XXS));
        // A long name elides with the whole name in its tooltip; the reason wraps.
        auto* name = new KLabel(QString{}, row);
        name->setObjectName(itemName_);
        name->setElidedText(entry.name);
        auto* reason = new KLabel(entry.reason, row, TypeRole::UiSmall);
        reason->setObjectName(itemReason_);
        reason->setWordWrap(true);
        reason->setTone(Color::Muted);
        column->addWidget(name);
        column->addWidget(reason);
        listRows_->addWidget(row);
    }
    listRows_->addStretch();
    list_->setVisible(!items.empty());

    detail_->hide();
    bar_->hide();
    counts_->show();
    cancel_->hide();
    close_->show();
    close_->setFocus();
    setDefaultRole(Role::Accept);
    adjustSize();
}

QString KTaskDialog::headingText() const {
    return heading_->toolTip().isEmpty() ? heading_->text() : heading_->toolTip();
}

QString KTaskDialog::detailText() const { return summarizing_ ? counts_->text() : detailFull_; }

void KTaskDialog::dismiss() {
    dismissing_ = true;
    close();
}

void KTaskDialog::requestStop() {
    // One request: a second Cancel, Escape or close while the work is stopping asks nothing more.
    if (summarizing_ || stopping_)
        return;
    stopping_ = true;
    showStopping(tr("Cancelling…"));
    emit stopRequested();
}

void KTaskDialog::reject() {
    // QDialog::closeEvent() closes through reject(), so a dismissal passes straight through.
    if (summarizing_ || dismissing_)
        KDialog::reject();
    else
        requestStop();
}

void KTaskDialog::closeEvent(QCloseEvent* event) {
    if (summarizing_ || dismissing_) {
        KDialog::closeEvent(event);
        return;
    }
    event->ignore();
    requestStop();
}

KTaskTiming::KTaskTiming(const std::chrono::milliseconds delay,
                         const std::chrono::milliseconds interval, QObject* parent)
    : QObject(parent) {
    delay_.setSingleShot(true);
    delay_.setInterval(delay);
    refresh_.setInterval(interval);
    connect(&delay_, &QTimer::timeout, this, [this] {
        emit showDue();
        refresh_.start();
    });
    connect(&refresh_, &QTimer::timeout, this, &KTaskTiming::refreshDue);
}

void KTaskTiming::begin() {
    refresh_.stop();
    delay_.start();
}

void KTaskTiming::finish() {
    delay_.stop();
    refresh_.stop();
}

} // namespace bloom::ui::kit
