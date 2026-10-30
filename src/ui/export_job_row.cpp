#include "export_job_row.hpp"

#include "export_detail_support.hpp"
#include "export_job_results.hpp"
#include "export_output_row.hpp"
#include "export_panel_text.hpp"
#include "properties_sections.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace bloom::ui::exportpanel {

using detail::named;
using detail::reasonLabel;
using detail::rgbOf;

namespace {

[[nodiscard]] QString tr(const char* source) {
    return QCoreApplication::translate("ExportJobRow", source);
}

kit::KPropertyRow* addRow(QVBoxLayout& layout, QWidget* parent, const QString& label,
                                        std::initializer_list<QWidget*> controls) {
    auto* row = new kit::KPropertyRow(properties::makeRowLabel(label, parent), nullptr, controls,
                                      parent);
    layout.addWidget(row);
    return row;
}

[[nodiscard]] QString countText(const host::ExportJobProgress& progress) {
    if (progress.framesTotal == 0)
        return {};
    const auto shown = progress.currentFrame.has_value()
                           ? std::min(progress.framesCompleted + 1, progress.framesTotal)
                           : progress.framesCompleted;
    return QStringLiteral("%1 / %2").arg(shown).arg(progress.framesTotal);
}

} // namespace

JobVerb contextualVerb(const host::ExportJobView& job) {
    using State = host::ExportJobState;
    switch (job.state) {
    case State::Ready:
        return JobVerb::Start;
    case State::Rendering:
        // Pause is the verb of a running job; a job that cannot pause offers Stop instead.
        return job.commands[host::ExportCommand::Pause].available ? JobVerb::Pause : JobVerb::Stop;
    case State::Paused:
        return JobVerb::Resume;
    case State::Waiting:
    case State::Preparing:
    case State::Pausing:
    case State::Verifying:
    case State::Stopping:
        return JobVerb::Stop;
    case State::Done:
    case State::DoneWithWarnings:
    case State::Failed:
    case State::Stopped:
        return JobVerb::Retry;
    }
    return JobVerb::Start;
}

QString verbText(const JobVerb verb) {
    switch (verb) {
    case JobVerb::Start:
        return tr("Start");
    case JobVerb::Pause:
        return tr("Pause");
    case JobVerb::Resume:
        return tr("Resume");
    case JobVerb::Stop:
        return tr("Stop");
    case JobVerb::Retry:
        return tr("Retry");
    }
    return {};
}

struct JobRow::Fields final {
    kit::KColorChip* label = nullptr;
    kit::KSwitch* enabled = nullptr;
    QWidget* nameBox = nullptr;
    kit::KLabel* name = nullptr;
    kit::KLineEdit* rename = nullptr;
    kit::KLabel* composition = nullptr;
    kit::KLabel* range = nullptr;
    QWidget* statusBox = nullptr;
    kit::KLabel* statusIcon = nullptr;
    kit::KLabel* status = nullptr;
    QWidget* progressBox = nullptr;
    kit::KProgressBar* bar = nullptr;
    kit::KLabel* count = nullptr;
    kit::KIconButton* menu = nullptr;
    kit::KIconButton* disclosure = nullptr;
    QWidget* content = nullptr;
    QWidget* lineOne = nullptr;
    QWidget* lineTwo = nullptr;
    QHBoxLayout* lineOneLayout = nullptr;
    QHBoxLayout* lineTwoLayout = nullptr;

    QWidget* body = nullptr;
    kit::KLabel* refusal = nullptr;
    kit::KLabel* lockNotice = nullptr;
    kit::KLabel* startReason = nullptr;
    kit::KLabel* jobReasons = nullptr;
    kit::KDropdown* rangeKind = nullptr;
    kit::KValueField* first = nullptr;
    kit::KValueField* last = nullptr;
    kit::KValueField* frame = nullptr;
    kit::KLabel* rangeSpan = nullptr;
    kit::KLabel* runLine = nullptr;
    kit::KPropertyRow* firstRow = nullptr;
    kit::KPropertyRow* lastRow = nullptr;
    kit::KPropertyRow* frameRow = nullptr;
    kit::KPropertyRow* spanRow = nullptr;
    kit::KPropertyRow* runRow = nullptr;
    QVBoxLayout* outputs = nullptr;
    kit::KButton* addOutput = nullptr;
    kit::KLabel* addReason = nullptr;
    QList<QWidget*> editors;
    std::vector<OutputRow*> outputRows;
    host::ExportJobState lastState = host::ExportJobState::Ready;
    bool wasAsking = false;
    bool haveState = false;
};

JobRow::JobRow(CompositionSession& session, ExportQueueAdapter& adapter,
               const document::ExportJobId job, QWidget* const parent)
    : QWidget(parent), session_(session), adapter_(adapter), job_(job), f_(std::make_unique<Fields>()) {
    setObjectName(QStringLiteral("exportJobRow"));
    setProperty("jobId", static_cast<qulonglong>(job.value()));
    setFocusPolicy(Qt::StrongFocus);
    build();
    setExpanded(true);
}

JobRow::~JobRow() = default;

void JobRow::build() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    buildHeader();
    layout->addWidget(header_);
    buildBody();
    layout->addWidget(f_->body);
}

void JobRow::buildHeader() {
    header_ = new kit::KRow(this);
    header_->setObjectName(QStringLiteral("exportJobHeader"));
    header_->installEventFilter(this);
    f_->disclosure = new kit::KIconButton(header_);
    f_->disclosure->setObjectName(QStringLiteral("exportJobDisclosure"));
    f_->disclosure->setProperty("disclosure", true);

    f_->content = new QWidget(header_);
    auto* lines = new QVBoxLayout(f_->content);
    lines->setContentsMargins(0, 0, 0, 0);
    lines->setSpacing(kit::px(kit::Spacing::XXS));
    f_->lineOne = new QWidget(f_->content);
    f_->lineTwo = new QWidget(f_->content);
    f_->lineOneLayout = new QHBoxLayout(f_->lineOne);
    f_->lineTwoLayout = new QHBoxLayout(f_->lineTwo);
    for (auto* line : {f_->lineOneLayout, f_->lineTwoLayout}) {
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(kit::px(kit::Spacing::S));
    }
    lines->addWidget(f_->lineOne);
    lines->addWidget(f_->lineTwo);

    f_->label = named(new kit::KColorChip(f_->content), QStringLiteral("exportJobLabel"),
                      tr("Label color"));
    f_->label->setControlSize(kit::KColorChip::ControlSize::Compact);
    f_->enabled = named(new kit::KSwitch(f_->content), QStringLiteral("exportJobEnabled"),
                        tr("Include in Start All"));
    f_->enabled->setToolTip(tr("Include in Start All"));

    f_->nameBox = new QWidget(f_->content);
    f_->nameBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* nameLayout = new QHBoxLayout(f_->nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(0);
    f_->name = new kit::KLabel(f_->nameBox);
    f_->name->setObjectName(QStringLiteral("exportJobName"));
    f_->name->installEventFilter(this);
    f_->rename = new kit::KLineEdit(f_->nameBox);
    f_->rename->setObjectName(QStringLiteral("exportJobRename"));
    f_->rename->setAccessibleName(tr("Job name"));
    f_->rename->useInlineEdit();
    f_->rename->hide();
    nameLayout->addWidget(f_->name, 1);
    nameLayout->addWidget(f_->rename, 1);

    f_->composition = new kit::KLabel(f_->content);
    f_->composition->setObjectName(QStringLiteral("exportJobComposition"));
    f_->composition->setTone(kit::Color::Muted);
    f_->range = new kit::KLabel(f_->content);
    f_->range->setObjectName(QStringLiteral("exportJobRange"));
    f_->range->setTone(kit::Color::Muted);

    f_->statusBox = new QWidget(f_->content);
    auto* statusLayout = new QHBoxLayout(f_->statusBox);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(kit::px(kit::Spacing::XS));
    f_->statusIcon = new kit::KLabel(f_->statusBox);
    f_->statusIcon->setObjectName(QStringLiteral("exportJobStatusIcon"));
    f_->statusIcon->setFixedWidth(kit::px(kit::Size::IconMedium));
    f_->status = new kit::KLabel(f_->statusBox);
    f_->status->setObjectName(QStringLiteral("exportJobStatus"));
    f_->status->setTone(kit::Color::Muted);
    statusLayout->addWidget(f_->statusIcon, 0);
    statusLayout->addWidget(f_->status, 1);
    f_->statusBox->setFixedWidth(kit::px(kit::Size::DropdownWidthWide) + kit::px(kit::Size::ToggleCell));

    f_->progressBox = new QWidget(f_->content);
    f_->progressBox->setObjectName(QStringLiteral("exportJobProgress"));
    auto* progressLayout = new QHBoxLayout(f_->progressBox);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(kit::px(kit::Spacing::S));
    f_->bar = new kit::KProgressBar(f_->progressBox);
    f_->bar->setObjectName(QStringLiteral("exportJobBar"));
    f_->bar->setAccessibleName(tr("Export progress"));
    f_->bar->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    f_->count = new kit::KLabel(QString{}, f_->progressBox, kit::TypeRole::UiSmall);
    f_->count->setObjectName(QStringLiteral("exportJobCount"));
    f_->count->setTone(kit::Color::Muted);
    f_->count->setFixedWidth(kit::px(kit::Size::DropdownWidthWide));
    progressLayout->addWidget(f_->bar, 0, Qt::AlignVCenter);
    progressLayout->addWidget(f_->count, 0);
    auto policy = f_->progressBox->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    f_->progressBox->setSizePolicy(policy);
    f_->progressBox->hide();

    button_ = new kit::KButton(tr("Start"), f_->content);
    button_->setObjectName(QStringLiteral("exportJobButton"));
    button_->setControlSize(kit::KButton::ControlSize::Compact);
    button_->setFixedWidth(kit::px(kit::Size::DropdownWidth) - kit::px(kit::Size::ToggleCell));
    f_->menu = new kit::KIconButton(f_->content);
    f_->menu->setObjectName(QStringLiteral("exportJobMenuButton"));
    f_->menu->setIcon(kit::icon(kit::IconId::ContextMenu, kit::IconRole::Chrome));
    f_->menu->setAccessibleName(tr("Job actions"));
    f_->menu->setToolTip(tr("More actions"));

    header_->setCells({f_->disclosure}, f_->content, {});
    header_->setLeadingInset(kit::px(kit::Spacing::XS));
    applyLayoutMode(false);

    connect(f_->disclosure, &QAbstractButton::clicked, this, [this] { setExpanded(!expanded_); });
    connect(button_, &kit::KButton::clicked, this, [this] { runVerb(); });
    connect(f_->menu, &QAbstractButton::clicked, this, [this] {
        showMenu(f_->menu->mapToGlobal(QPoint(0, f_->menu->height())));
    });
    connect(f_->enabled, &QAbstractButton::toggled, this, [this](const bool on) {
        if (!refreshing_)
            report(adapter_.editJob(job_, jobEdit([&](auto& edit) { edit.enabled = on; })));
    });
    connect(f_->label, &kit::KColorChip::editFinished, this, [this] {
        if (refreshing_)
            return;
        const auto chosen = f_->label->color().toQColor();
        const auto* view = findJob();
        if (view != nullptr && view->record.labelColor == rgbOf(chosen))
            return;
        report(adapter_.editJob(job_, jobEdit([&](auto& edit) {
                                    edit.labelColor = std::optional<std::array<std::uint8_t, 3>>(rgbOf(chosen));
                                })));
    });
    if (auto* inlineEdit = f_->rename->inlineEdit()) {
        connect(inlineEdit, &kit::KInlineEdit::accepted, this, [this](const QString& text) {
            // Hiding the editor ends its edit once more: only the first ending counts.
            if (!renaming_)
                return;
            const auto* view = findJob();
            const auto name = text.toStdString();
            endRename();
            if (view != nullptr && name != view->record.name)
                report(adapter_.editJob(job_, jobEdit([&](auto& edit) { edit.name = name; })));
        });
        connect(inlineEdit, &kit::KInlineEdit::refused, this, [this] { endRename(); });
    }
    f_->editors = {f_->label, f_->enabled};
}

void JobRow::buildBody() {
    f_->body = new QWidget(this);
    f_->body->setObjectName(QStringLiteral("exportJobBody"));
    auto* body = new QVBoxLayout(f_->body);
    body->setContentsMargins(kit::px(kit::Size::ToggleCell), kit::px(kit::Spacing::XS),
                             kit::px(kit::Spacing::XS), kit::px(kit::Spacing::S));
    body->setSpacing(kit::px(kit::Spacing::XS));

    // Reasons first: what the artist needs to know before touching anything.
    f_->refusal = reasonLabel(f_->body, QStringLiteral("exportRefusal"), kit::Color::Muted);
    f_->refusal->hide();
    f_->lockNotice = reasonLabel(f_->body, QStringLiteral("exportLockNotice"), kit::Color::Muted);
    f_->lockNotice->setText(tr("Settings are locked while this job renders. Stop it to change them."));
    f_->lockNotice->hide();
    f_->startReason = reasonLabel(f_->body, QStringLiteral("exportStartReason"), kit::Color::Muted);
    f_->startReason->hide();
    f_->jobReasons = reasonLabel(f_->body, QStringLiteral("exportJobReasons"), kit::Color::Muted);
    f_->jobReasons->hide();
    body->addWidget(f_->refusal);
    body->addWidget(f_->lockNotice);
    body->addWidget(f_->startReason);
    body->addWidget(f_->jobReasons);

    render_ = properties::makeSection(body, f_->body, QStringLiteral("exportRenderSection"),
                                      tr("Render"), QStringLiteral("export/job/render/collapsed"));
    render_->setBodyPadding(kit::px(kit::Spacing::XS));
    render_->setResetEnabled(false);
    auto* rows = render_->bodyLayout();
    QWidget* parent = render_->body();

    f_->rangeKind = named(new kit::KDropdown(parent), QStringLiteral("exportRangeKind"), tr("Range"));
    f_->rangeKind->addItem(tr("Work area"), static_cast<int>(document::ExportRangeKind::WorkArea));
    f_->rangeKind->addItem(tr("Whole composition"),
                           static_cast<int>(document::ExportRangeKind::Composition));
    f_->rangeKind->addItem(tr("Custom frames"), static_cast<int>(document::ExportRangeKind::Custom));
    f_->rangeKind->addItem(tr("One frame"), static_cast<int>(document::ExportRangeKind::Frame));
    f_->rangeSpan = reasonLabel(parent, QStringLiteral("exportRangeSpan"), kit::Color::Muted);
    f_->rangeSpan->setWordWrap(false);
    f_->rangeSpan->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    f_->rangeSpan->setMinimumWidth(0);
    addRow(*rows, parent, tr("Range"), {f_->rangeKind, f_->rangeSpan});
    const auto frameField = [parent](const QString& name, const QString& label) {
        auto* field = named(new kit::KValueField(parent), name, label);
        field->setIntegerMode(true);
        field->setIntegerRange(0, 100'000'000);
        field->setUnit(tr("f"));
        field->setFixedWidth(kit::px(kit::Size::DropdownWidth));
        return field;
    };
    f_->first = frameField(QStringLiteral("exportRangeFirst"), tr("First frame"));
    f_->last = frameField(QStringLiteral("exportRangeLast"), tr("Last frame"));
    f_->frame = frameField(QStringLiteral("exportRangeFrame"), tr("Frame"));
    f_->firstRow = addRow(*rows, parent, tr("First frame"), {f_->first});
    f_->lastRow = addRow(*rows, parent, tr("Last frame"), {f_->last});
    f_->frameRow = addRow(*rows, parent, tr("Frame"), {f_->frame});
    f_->runLine = reasonLabel(parent, QStringLiteral("exportRunLine"), kit::Color::Muted);
    f_->runLine->setWordWrap(false);
    f_->runLine->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    f_->runLine->setMinimumWidth(0);
    f_->runRow = addRow(*rows, parent, QString{}, {f_->runLine});
    f_->runRow->hide();
    f_->editors.push_back(f_->rangeKind);
    f_->editors.push_back(f_->first);
    f_->editors.push_back(f_->last);
    f_->editors.push_back(f_->frame);

    auto* outputsHolder = new QWidget(f_->body);
    f_->outputs = new QVBoxLayout(outputsHolder);
    f_->outputs->setContentsMargins(0, 0, 0, 0);
    f_->outputs->setSpacing(0);
    body->addWidget(outputsHolder);

    auto* addRowWidget = new QWidget(f_->body);
    auto* addLayout = new QHBoxLayout(addRowWidget);
    addLayout->setContentsMargins(0, 0, 0, 0);
    addLayout->setSpacing(kit::px(kit::Spacing::S));
    f_->addOutput = new kit::KButton(kit::IconId::Add, tr("Add output"), addRowWidget);
    f_->addOutput->setObjectName(QStringLiteral("exportAddOutputButton"));
    f_->addOutput->setControlSize(kit::KButton::ControlSize::Compact);
    f_->addOutput->setToolTip(tr("Render this job in another format too."));
    f_->addReason = reasonLabel(addRowWidget, QStringLiteral("exportAddOutputReason"), kit::Color::Muted);
    f_->addReason->hide();
    f_->addOutput->setFixedWidth(f_->addOutput->sizeHint().width());
    addLayout->addWidget(f_->addOutput, 0);
    addLayout->addWidget(f_->addReason, 1);
    body->addWidget(addRowWidget);

    results_ = new JobResults(adapter_, job_, f_->body);
    body->addWidget(results_);

    connect(f_->rangeKind, &kit::KDropdown::activated, this, [this] {
        const auto* view = findJob();
        if (refreshing_ || view == nullptr)
            return;
        document::ExportRange range = view->record.render.range;
        range.kind = static_cast<document::ExportRangeKind>(f_->rangeKind->currentData().toInt());
        if (range.kind == document::ExportRangeKind::Custom ||
            range.kind == document::ExportRangeKind::Frame) {
            // Start from what the job renders now.
            range.first = view->resolvedRange ? view->resolvedRange->first : range.first;
            range.last = range.kind == document::ExportRangeKind::Frame
                             ? range.first
                             : (view->resolvedRange ? view->resolvedRange->last
                                                    : std::max(range.first, range.last));
        }
        report(adapter_.editJob(job_, jobEdit([&](auto& edit) { edit.range = range; })));
    });
    for (auto* field : {f_->first, f_->last, f_->frame})
        connect(field, &kit::KValueField::editFinished, this, [this] { commitRange(); });
    connect(f_->addOutput, &kit::KButton::clicked, this, [this] { addOutputMenu(); });
}

void JobRow::commitRange() {
    const auto* view = findJob();
    if (refreshing_ || view == nullptr)
        return;
    document::ExportRange range = view->record.render.range;
    if (range.kind == document::ExportRangeKind::Frame) {
        range.first = static_cast<std::uint64_t>(f_->frame->integerValue());
        range.last = range.first;
    } else {
        range.first = static_cast<std::uint64_t>(f_->first->integerValue());
        range.last = static_cast<std::uint64_t>(f_->last->integerValue());
    }
    if (range.kind == document::ExportRangeKind::Custom && range.last < range.first) {
        f_->refusal->setText(tr("The last frame comes before the first frame."));
        f_->refusal->show();
        return;
    }
    report(adapter_.editJob(job_, jobEdit([&](auto& edit) { edit.range = range; })));
}

void JobRow::report(const ExportEditResult& result) {
    if (result.accepted) {
        f_->refusal->hide();
        return;
    }
    f_->refusal->setText(result.reason);
    f_->refusal->setVisible(!result.reason.isEmpty());
}

const host::ExportJobView* JobRow::findJob() const {
    const auto snapshot = adapter_.snapshot();
    if (!snapshot)
        return nullptr;
    for (const auto& job : snapshot->jobs)
        if (job.id == job_)
            return &job;
    return nullptr;
}

void JobRow::setExpanded(const bool expanded) {
    const bool changed = expanded != expanded_;
    expanded_ = expanded;
    f_->body->setVisible(expanded);
    f_->disclosure->setIcon(kit::icon(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight,
                                      kit::IconRole::Chrome));
    f_->disclosure->setAccessibleName(expanded ? tr("Hide job settings") : tr("Show job settings"));
    if (changed)
        Q_EMIT expandedChanged(static_cast<qulonglong>(job_.value()), expanded);
}

void JobRow::setCurrent(const bool current) {
    current_ = current;
    header_->setRowState(index_, current);
}

void JobRow::setPosition(const int index, const int count) {
    index_ = index;
    count_ = count;
    header_->setRowState(index_, current_);
}

OutputRow* JobRow::outputRow(const document::ExportOutputId output) const {
    for (auto* row : f_->outputRows)
        if (row->output() == output)
            return row;
    return nullptr;
}

std::vector<OutputRow*> JobRow::outputRows() const { return f_->outputRows; }

int JobRow::singleLineWidth() const {
    // What the one-line header needs: every fixed column, a name that can still be read, and the
    // gaps between them.
    const int gaps = kit::px(kit::Spacing::S) * 9;
    const int nameMinimum = kit::px(kit::Size::ToggleCell) + kit::px(kit::Size::ToggleCell);
    return kit::px(kit::Size::ToggleCell) + f_->label->sizeHint().width() +
           f_->enabled->sizeHint().width() + nameMinimum + kit::px(kit::Size::DropdownWidth) +
           kit::px(kit::Size::DropdownWidthWide) + f_->statusBox->minimumWidth() +
           kit::px(kit::Size::DropdownWidth) + kit::px(kit::Size::DropdownWidth) +
           button_->minimumWidth() + f_->menu->minimumWidth() + gaps;
}

void JobRow::resizeEvent(QResizeEvent* const event) {
    QWidget::resizeEvent(event);
    const bool narrow = width() < singleLineWidth();
    if (narrow != narrow_)
        applyLayoutMode(narrow);
}

void JobRow::applyColumns() {
    // setElidedText frees a label's minimum width, so the wide header's columns are set again after
    // every text change.
    const auto fixedColumn = [this](QWidget* label, const int width) {
        // An Ignored label has no width of its own: the wide columns are Fixed.
        if (narrow_) {
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            label->setMinimumWidth(0);
            label->setMaximumWidth(QWIDGETSIZE_MAX);
        } else {
            label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            label->setFixedWidth(width);
        }
    };
    fixedColumn(f_->composition, kit::px(kit::Size::DropdownWidth));
    fixedColumn(f_->range, kit::px(kit::Size::DropdownWidthWide));
    f_->count->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    f_->count->setFixedWidth(kit::px(kit::Size::DropdownWidth));
}

void JobRow::applyLayoutMode(const bool narrow) {
    narrow_ = narrow;
    for (auto* layout : {f_->lineOneLayout, f_->lineTwoLayout})
        while (auto* item = layout->takeAt(0))
            delete item;
    applyColumns();
    auto* one = f_->lineOneLayout;
    one->addWidget(f_->label, 0, Qt::AlignVCenter);
    one->addWidget(f_->enabled, 0, Qt::AlignVCenter);
    one->addWidget(f_->nameBox, 1);
    if (!narrow) {
        one->addWidget(f_->composition, 0);
        one->addWidget(f_->range, 0);
        one->addWidget(f_->statusBox, 0);
        one->addWidget(f_->progressBox, 0);
        one->addWidget(button_, 0, Qt::AlignVCenter);
        one->addWidget(f_->menu, 0, Qt::AlignVCenter);
        f_->lineTwo->hide();
        f_->count->show();
        header_->setFixedHeight(kit::px(kit::Size::ListRow));
    } else {
        one->addWidget(f_->statusBox, 0);
        auto* two = f_->lineTwoLayout;
        two->addWidget(f_->composition, 1);
        two->addWidget(f_->range, 1);
        two->addWidget(f_->progressBox, 0);
        two->addWidget(button_, 0, Qt::AlignVCenter);
        two->addWidget(f_->menu, 0, Qt::AlignVCenter);
        f_->lineTwo->show();
        f_->count->hide();
        header_->setFixedHeight(kit::px(kit::Size::Control) * 2 + kit::px(kit::Spacing::S));
    }
    // Moving a widget between layouts hides it: show what belongs to the header again.
    for (auto* moved : {static_cast<QWidget*>(f_->label), static_cast<QWidget*>(f_->enabled), f_->nameBox,
                        static_cast<QWidget*>(f_->composition), static_cast<QWidget*>(f_->range),
                        f_->statusBox, static_cast<QWidget*>(button_), static_cast<QWidget*>(f_->menu)})
        moved->show();
    // The wide header keeps the progress column so the buttons line up; the narrow one gives it back.
    auto progressPolicy = f_->progressBox->sizePolicy();
    progressPolicy.setRetainSizeWhenHidden(!narrow);
    f_->progressBox->setSizePolicy(progressPolicy);
    f_->progressBox->setVisible(progressWanted_);
    // The widgets changed layouts: lay both lines out now rather than at the next event.
    for (auto* line : {f_->lineOneLayout, f_->lineTwoLayout}) {
        line->invalidate();
        line->activate();
    }
    f_->content->layout()->invalidate();
    f_->content->layout()->activate();
}

bool JobRow::eventFilter(QObject* const watched, QEvent* const event) {
    if (watched == f_->name && event->type() == QEvent::MouseButtonDblClick) {
        startRename();
        return true;
    }
    if (watched == header_) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                setFocus(Qt::MouseFocusReason);
                Q_EMIT activated(static_cast<qulonglong>(job_.value()));
            }
            break;
        case QEvent::MouseButtonDblClick:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                setExpanded(!expanded_);
                return true;
            }
            break;
        case QEvent::ContextMenu: {
            Q_EMIT activated(static_cast<qulonglong>(job_.value()));
            showMenu(static_cast<QContextMenuEvent*>(event)->globalPos());
            return true;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void JobRow::keyPressEvent(QKeyEvent* const event) {
    if (event->key() == Qt::Key_F2) {
        startRename();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void JobRow::startRename() {
    const auto* view = findJob();
    if (view == nullptr || !view->commands[host::ExportCommand::EditJob].available)
        return;
    f_->rename->setText(qs(view->record.name));
    f_->rename->setPlaceholderText(qs(view->displayName));
    renaming_ = true;
    f_->name->hide();
    f_->rename->show();
    f_->rename->setFocus(Qt::OtherFocusReason);
    f_->rename->selectAll();
}

void JobRow::endRename() {
    renaming_ = false;
    f_->rename->hide();
    f_->name->show();
}

void JobRow::runVerb() {
    switch (verb_) {
    case JobVerb::Start:
        (void)adapter_.start(job_);
        break;
    case JobVerb::Pause:
        (void)adapter_.pause(job_);
        break;
    case JobVerb::Resume:
        (void)adapter_.resume(job_);
        break;
    case JobVerb::Stop:
        (void)adapter_.stop(job_);
        break;
    case JobVerb::Retry:
        (void)adapter_.retry(job_);
        break;
    }
}

void JobRow::addOutputMenu() {
    const auto snapshot = adapter_.snapshot();
    const auto catalogue = adapter_.catalogue();
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("exportAddOutputMenu"));
    menu->setToolTipsVisible(true);
    struct Group {
        host::ExportPresetCategory category;
        QString title;
    };
    const Group groups[] = {{host::ExportPresetCategory::Video, tr("Video")},
                            {host::ExportPresetCategory::VfxSequences, tr("Image sequences")},
                            {host::ExportPresetCategory::Audio, tr("Audio")},
                            {host::ExportPresetCategory::User, tr("My presets")}};
    if (catalogue) {
        for (const auto& group : groups) {
            bool header = false;
            for (const auto& preset : catalogue->presets) {
                if (preset.category != group.category)
                    continue;
                if (!header) {
                    menu->addSection(group.title);
                    header = true;
                }
                auto* action = menu->addAction(qs(preset.displayName));
                action->setToolTip(qs(preset.description));
                const auto id = qs(preset.id);
                connect(action, &QAction::triggered, this, [this, id] { addOutputWithFormat(id); });
            }
        }
    }
    menu->exec(f_->addOutput->mapToGlobal(QPoint(0, f_->addOutput->height())));
}

void JobRow::addOutputWithFormat(const QString& presetId) {
    const auto result = adapter_.addOutput(job_, presetId.toStdString());
    report(result);
}

std::unique_ptr<QMenu> JobRow::buildMenu() {
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("exportContextMenu"));
    menu->setToolTipsVisible(true);
    const auto snapshot = adapter_.snapshot();
    const auto* view = findJob();
    if (view == nullptr)
        return menu;
    const auto add = [&](const QString& text, const char* name,
                         const host::ExportCommandAvailability& availability, auto&& slot) {
        auto* action = menu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setEnabled(availability.available);
        if (!availability.available)
            action->setToolTip(qs(availability.reason));
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };
    const auto id = job_;
    add(tr("Start"), "exportStartAction", view->commands[host::ExportCommand::Start],
        [this, id] { (void)adapter_.start(id); });
    add(tr("Pause"), "exportPauseAction", view->commands[host::ExportCommand::Pause],
        [this, id] { (void)adapter_.pause(id); });
    add(tr("Resume"), "exportResumeAction", view->commands[host::ExportCommand::Resume],
        [this, id] { (void)adapter_.resume(id); });
    add(tr("Stop"), "exportStopAction", view->commands[host::ExportCommand::Stop],
        [this, id] { (void)adapter_.stop(id); });
    add(tr("Retry"), "exportRetryAction", view->commands[host::ExportCommand::Retry],
        [this, id] { (void)adapter_.retry(id); });
    menu->addSeparator();
    add(tr("Duplicate"), "exportDuplicateAction", view->commands[host::ExportCommand::DuplicateJob],
        [this, id] { (void)adapter_.duplicateJob(id); });
    auto move = view->commands[host::ExportCommand::MoveJob];
    auto up = move;
    auto down = move;
    if (index_ <= 0) {
        up.available = false;
        up.reason = tr("This job is already first.").toStdString();
    }
    if (index_ >= count_ - 1) {
        down.available = false;
        down.reason = tr("This job is already last.").toStdString();
    }
    add(tr("Move up"), "exportMoveUpAction", up,
        [this] { Q_EMIT moveRequested(static_cast<qulonglong>(job_.value()), -1); });
    add(tr("Move down"), "exportMoveDownAction", down,
        [this] { Q_EMIT moveRequested(static_cast<qulonglong>(job_.value()), 1); });
    auto clear = view->commands[host::ExportCommand::EditJob];
    if (clear.available && !view->record.labelColor.has_value()) {
        clear.available = false;
        clear.reason = tr("This job has no label color.").toStdString();
    }
    add(tr("Remove label color"), "exportClearLabelAction", clear, [this] {
        report(adapter_.editJob(job_, jobEdit([](auto& edit) {
                                    edit.labelColor = std::optional<std::array<std::uint8_t, 3>>();
                                })));
    });
    add(tr("Remove"), "exportRemoveAction", view->commands[host::ExportCommand::RemoveJob],
        [this, id] { (void)adapter_.removeJob(id); });
    menu->addSeparator();
    const auto copy = [this](const host::ExportScriptFormat format) {
        if (const auto* current = findJob())
            QGuiApplication::clipboard()->setText(qs(adapter_.jobScript(current->record, format)));
    };
    menu->addAction(tr("Copy as bloom-cli Command"), this,
                    [copy] { copy(host::ExportScriptFormat::CliJson); })
        ->setObjectName(QStringLiteral("exportCopyCliAction"));
    menu->addAction(tr("Copy as Python"), this, [copy] { copy(host::ExportScriptFormat::Python); })
        ->setObjectName(QStringLiteral("exportCopyPythonAction"));
    return menu;
}

void JobRow::showMenu(const QPoint globalPosition) {
    const auto menu = buildMenu();
    menu->exec(globalPosition);
}

QString JobRow::nameText() const { return f_->name->property("fullText").toString(); }
QString JobRow::compositionText() const { return f_->composition->property("fullText").toString(); }
QString JobRow::rangeText() const { return f_->range->property("fullText").toString(); }
QString JobRow::statusText() const { return f_->status->property("fullText").toString(); }
QString JobRow::progressText() const { return f_->count->property("fullText").toString(); }
bool JobRow::progressShown() const { return !f_->progressBox->isHidden(); }
QString JobRow::reasonsText() const {
    QStringList lines;
    for (const auto* label : {f_->refusal, f_->lockNotice, f_->startReason, f_->jobReasons, f_->addReason})
        if (!label->isHidden() && !label->text().isEmpty())
            lines.push_back(label->text());
    for (const auto* output : f_->outputRows)
        if (const auto text = output->reasonsText(); !text.isEmpty())
            lines.push_back(text);
    return lines.join(QLatin1Char('\n'));
}

void JobRow::update(const host::ExportJobView& job,
                    const host::ExportPresetCatalogueSnapshot* catalogue,
                    const ExportColourChoices& colours) {
    refreshing_ = true;
    catalogue_ = catalogue;
    colours_ = colours;
    const auto* composition = session_.snapshot().project().findComposition(job.record.composition);
    const auto status = jobStatus(job);
    const auto& edit = job.commands[host::ExportCommand::EditJob];

    // Header.
    f_->name->setElidedText(qs(job.displayName));
    f_->name->setProperty("fullText", qs(job.displayName));
    const auto compositionName = composition ? qs(composition->name()) : tr("Composition removed");
    f_->composition->setElidedText(compositionName);
    f_->composition->setProperty("fullText", compositionName);
    f_->range->setElidedText(exportpanel::rangeText(job));
    f_->range->setProperty("fullText", exportpanel::rangeText(job));
    f_->range->setToolTip(composition ? rangeTimecodeText(job, composition->format().frameRate())
                                      : QString{});
    f_->status->setElidedText(status.text);
    f_->status->setProperty("fullText", status.text);
    f_->status->setToolTip(status.detail.isEmpty() ? status.text : status.detail);
    f_->status->setTone(toneColor(status.tone));
    {
        const auto glyph = status.tone == StatusTone::Success   ? std::optional(kit::IconId::Check)
                           : status.tone == StatusTone::Warning ? std::optional(kit::IconId::Info)
                                                                : std::nullopt;
        if (glyph.has_value())
            f_->statusIcon->setPixmap(kit::icon(*glyph, kit::IconRole::Chrome, kit::Color::Muted)
                                          .pixmap(kit::px(kit::Size::IconMedium)));
        else
            f_->statusIcon->clear();
        f_->statusIcon->setVisible(glyph.has_value());
    }
    f_->enabled->setChecked(job.record.enabled);
    if (job.record.labelColor.has_value()) {
        const auto& rgb = *job.record.labelColor;
        f_->label->setColor(kit::KColor::fromQColor(QColor(rgb[0], rgb[1], rgb[2])));
    } else {
        // No label: a fully transparent chip reads as the empty state, as a layer's does.
        f_->label->setColor(kit::KColor::fromRgba(0.0F, 0.0F, 0.0F, 0.0F));
    }
    for (auto* widget : f_->editors) {
        widget->setEnabled(edit.available);
        widget->setToolTip(edit.available ? QString{} : qs(edit.reason));
    }
    f_->enabled->setToolTip(edit.available ? tr("Include in Start All") : qs(edit.reason));

    // Progress, only while the job runs.
    const bool progressShown = job.state == host::ExportJobState::Preparing ||
                               job.state == host::ExportJobState::Rendering ||
                               job.state == host::ExportJobState::Pausing ||
                               job.state == host::ExportJobState::Paused ||
                               job.state == host::ExportJobState::Verifying;
    progressWanted_ = progressShown;
    f_->progressBox->setVisible(progressShown);
    if (job.progress.framesTotal > 0 && !isWaitingForViewer(job))
        f_->bar->setProgress(std::min(job.progress.framesCompleted, job.progress.framesTotal),
                             job.progress.framesTotal);
    else
        f_->bar->setBusy();
    auto count = countText(job.progress);
    if (const auto remaining = remainingText(job.progress); !remaining.isEmpty())
        count += QStringLiteral(" · ") + remaining;
    f_->count->setElidedText(count);
    f_->count->setProperty("fullText", count);
    applyColumns();
    auto runLine = frameProgressText(job.progress);
    if (!runLine.isEmpty())
        runLine += QStringLiteral(" · ") + elapsedText(job.progress);
    if (const auto remaining = remainingText(job.progress); !remaining.isEmpty())
        runLine += QStringLiteral(" · ") + remaining;
    f_->runLine->setText(runLine);
    f_->runRow->setVisible(progressShown && !runLine.isEmpty());
    f_->progressBox->setToolTip(runLine);

    // The one job button.
    verb_ = contextualVerb(job);
    const auto command = [&]() -> const host::ExportCommandAvailability& {
        switch (verb_) {
        case JobVerb::Start:
            return job.commands[host::ExportCommand::Start];
        case JobVerb::Pause:
            return job.commands[host::ExportCommand::Pause];
        case JobVerb::Resume:
            return job.commands[host::ExportCommand::Resume];
        case JobVerb::Stop:
            return job.commands[host::ExportCommand::Stop];
        case JobVerb::Retry:
            return job.commands[host::ExportCommand::Retry];
        }
        return job.commands[host::ExportCommand::Start];
    }();
    button_->setText(verbText(verb_));
    button_->setEnabled(command.available);
    button_->setToolTip(command.available ? QString{} : qs(command.reason));
    button_->setProperty("verb", static_cast<int>(verb_));

    // Body: the reasons that are shown rather than hidden in a tooltip.
    const bool locked = edit.code == host::ExportUnavailableCode::LockedWhileRunning;
    f_->lockNotice->setVisible(locked);
    const auto& start = job.commands[host::ExportCommand::Start];
    // A replace question and a pending check have their own words on the output; every other
    // reason Start cannot run is said here, once, in full.
    const bool saysReason = job.state == host::ExportJobState::Ready && !start.available &&
                            start.code != host::ExportUnavailableCode::NeedsReplaceConfirmation &&
                            start.code != host::ExportUnavailableCode::DestinationCheckPending &&
                            start.code != host::ExportUnavailableCode::PlatformNotSupported;
    f_->startReason->setText(saysReason ? qs(start.reason) : QString{});
    f_->startReason->setVisible(saysReason);
    QStringList reasons;
    for (const auto& issue : orderedIssues(job.preflight))
        if (!reasons.contains(qs(issue.reason)) && (!saysReason || qs(issue.reason) != qs(start.reason)))
            reasons.push_back(qs(issue.reason));
    f_->jobReasons->setText(reasons.join(QLatin1Char(' ')));
    f_->jobReasons->setVisible(!reasons.isEmpty());

    // Render.
    const auto kind = job.record.render.range.kind;
    {
        const QSignalBlocker block(f_->rangeKind);
        f_->rangeKind->setCurrentIndex(std::max(0, f_->rangeKind->findData(static_cast<int>(kind))));
    }
    f_->first->setIntegerValue(static_cast<std::int64_t>(job.record.render.range.first));
    f_->last->setIntegerValue(static_cast<std::int64_t>(job.record.render.range.last));
    f_->frame->setIntegerValue(static_cast<std::int64_t>(job.record.render.range.first));
    f_->firstRow->setVisible(kind == document::ExportRangeKind::Custom);
    f_->lastRow->setVisible(kind == document::ExportRangeKind::Custom);
    f_->frameRow->setVisible(kind == document::ExportRangeKind::Frame);
    QString span;
    if (composition != nullptr && job.resolvedRange.has_value()) {
        const auto frames = job.resolvedRange->last - job.resolvedRange->first + 1;
        span = tr("%1 · %2 frames").arg(rangeTimecodeText(job, composition->format().frameRate()))
                   .arg(frames);
    }
    f_->rangeSpan->setText(span);

    // Outputs: one row each, in order, created and removed with the snapshot.
    std::vector<OutputRow*> rows;
    for (const auto& output : job.outputs) {
        OutputRow* row = outputRow(output.id);
        const bool created = row == nullptr;
        if (created) {
            row = new OutputRow(session_, adapter_, job_, output.id, f_->outputs->parentWidget());
            f_->outputs->addWidget(row);
            // A job's first output stays folded; an output added later opens at once.
            if (f_->haveState)
                row->setExpanded(true);
        }
        row->update(job, output, catalogue, colours);
        rows.push_back(row);
    }
    for (auto* existing : f_->outputRows)
        if (std::ranges::find(rows, existing) == rows.end()) {
            f_->outputs->removeWidget(existing);
            delete existing;
        }
    f_->outputRows = rows;

    bool asking = false;
    for (std::size_t i = 0; i < job.outputs.size(); ++i)
        if (awaitsReplaceConfirmation(job.outputs[i])) {
            asking = true;
            if (!f_->wasAsking)
                rows[i]->setExpanded(true);
        }
    if (asking && !f_->wasAsking)
        setExpanded(true);
    f_->wasAsking = asking;

    const auto& add = job.commands[host::ExportCommand::AddOutput];
    f_->addOutput->setEnabled(add.available);
    f_->addReason->setText(add.available ? QString{} : qs(add.reason));
    f_->addReason->setVisible(!add.available);

    // A job that just finished folds away; one that failed stays open to show why.
    if (f_->haveState && f_->lastState != job.state &&
        (job.state == host::ExportJobState::Done || job.state == host::ExportJobState::DoneWithWarnings))
        setExpanded(false);
    f_->lastState = job.state;
    f_->haveState = true;

    results_->update(job, catalogue);
    refreshing_ = false;
}

} // namespace bloom::ui::exportpanel
