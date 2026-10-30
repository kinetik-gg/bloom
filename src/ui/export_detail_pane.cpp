#include "export_detail_fields.hpp"
#include "export_detail_support.hpp"
#include "export_panel_text.hpp"

#include "properties_sections.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QDir>
#include <QSignalBlocker>
#include <QUrl>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::exportpanel {

using detail::named;
using detail::reasonLabel;
using detail::rgbOf;
ExportDetailPane::ExportDetailPane(CompositionSession& session, ExportQueueAdapter& adapter,
                                   QWidget* const parent)
    : QWidget(parent), session_(session), adapter_(adapter), f_(std::make_unique<Fields>()) {
    setObjectName(QStringLiteral("exportDetailPane"));
    build();
    hide();
}

ExportDetailPane::~ExportDetailPane() = default;

kit::KPropertyRow* ExportDetailPane::addRow(QVBoxLayout& layout, const QString& label,
                                            std::initializer_list<QWidget*> controls,
                                            const QString& objectName) {
    auto* row = new kit::KPropertyRow(properties::makeRowLabel(label, this), nullptr, controls, this);
    if (!objectName.isEmpty())
        row->setObjectName(objectName);
    layout.addWidget(row);
    return row;
}

void ExportDetailPane::build() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::Gutter));
    buildRender(*layout);
    buildOutputs(*layout);
    buildPostRender(*layout);
    buildResults(*layout);
}

void ExportDetailPane::buildRender(QVBoxLayout& layout) {
    auto* section = properties::makeSection(&layout, this, QStringLiteral("exportRenderSection"),
                                            tr("Render"),
                                            QStringLiteral("export/sections/render/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    section->setResetEnabled(false);
    auto* rows = section->bodyLayout();

    // The job's own actions. Each reads its typed availability; the reason is the tooltip.
    const auto action = [&](const QString& text, const QString& name) {
        auto* button = new kit::KButton(text, this);
        button->setObjectName(name);
        return button;
    };
    f_->start = action(tr("Start"), QStringLiteral("exportJobStartButton"));
    f_->pauseResume = action(tr("Pause"), QStringLiteral("exportJobPauseButton"));
    f_->stop = action(tr("Stop"), QStringLiteral("exportJobStopButton"));
    f_->retry = action(tr("Retry"), QStringLiteral("exportJobRetryButton"));
    (void)addRow(*rows, QString{}, {f_->start, f_->pauseResume, f_->stop, f_->retry});
    f_->refusal = reasonLabel(this, QStringLiteral("exportRefusal"), kit::Color::Warn);
    f_->refusal->hide();
    rows->addWidget(f_->refusal);

    f_->name = named(new kit::KLineEdit(this), QStringLiteral("exportJobName"), tr("Job name"));
    addRow(*rows, tr("Name"), {f_->name});
    f_->rangeKind = named(new kit::KDropdown(this), QStringLiteral("exportRangeKind"), tr("Range"));
    f_->rangeKind->addItem(tr("Work area"), static_cast<int>(document::ExportRangeKind::WorkArea));
    f_->rangeKind->addItem(tr("Composition"), static_cast<int>(document::ExportRangeKind::Composition));
    f_->rangeKind->addItem(tr("Custom frames"), static_cast<int>(document::ExportRangeKind::Custom));
    f_->rangeKind->addItem(tr("Single frame"), static_cast<int>(document::ExportRangeKind::Frame));
    addRow(*rows, tr("Range"), {f_->rangeKind});
    const auto frameField = [this](const QString& name, const QString& label) {
        auto* field = named(new kit::KValueField(this), name, label);
        field->setIntegerMode(true);
        field->setIntegerRange(0, 100'000'000);
        field->setUnit(tr("f"));
        return field;
    };
    f_->first = frameField(QStringLiteral("exportRangeFirst"), tr("First frame"));
    f_->last = frameField(QStringLiteral("exportRangeLast"), tr("Last frame"));
    f_->firstRow = addRow(*rows, tr("First frame"), {f_->first});
    f_->lastRow = addRow(*rows, tr("Last frame"), {f_->last});
    f_->rangeSpan = reasonLabel(this, QStringLiteral("exportRangeSpan"), kit::Color::Muted);
    f_->rangeSpanRow = addRow(*rows, QString{}, {f_->rangeSpan});
    f_->rangeReason = reasonLabel(this, QStringLiteral("exportRangeReason"), kit::Color::Warn);
    f_->rangeReason->hide();
    rows->addWidget(f_->rangeReason);
    f_->jobEnabled = named(new kit::KSwitch(this), QStringLiteral("exportJobEnabled"),
                           tr("Included by Start All"));
    addRow(*rows, tr("Enabled"), {f_->jobEnabled});
    f_->label = named(new kit::KColorChip(this), QStringLiteral("exportJobLabel"),
                      tr("Label colour"));
    f_->clearLabel = new kit::KButton(tr("None"), this);
    f_->clearLabel->setObjectName(QStringLiteral("exportJobLabelClear"));
    f_->labelRow = addRow(*rows, tr("Label"), {f_->label, f_->clearLabel});

    f_->editors = {f_->name,   f_->rangeKind, f_->first, f_->last,
                   f_->jobEnabled, f_->label, f_->clearLabel};

    connect(f_->start, &kit::KButton::clicked, this, [this] {
        if (job_)
            (void)adapter_.start(*job_);
    });
    connect(f_->pauseResume, &kit::KButton::clicked, this, [this] {
        const auto* view = currentJob();
        if (view == nullptr)
            return;
        (void)(view->state == host::ExportJobState::Paused ? adapter_.resume(view->id)
                                                           : adapter_.pause(view->id));
    });
    connect(f_->stop, &kit::KButton::clicked, this, [this] {
        if (job_)
            (void)adapter_.stop(*job_);
    });
    connect(f_->retry, &kit::KButton::clicked, this, [this] {
        if (job_)
            (void)adapter_.retry(*job_);
    });
    const auto report = [this](const ExportEditResult& result) {
        if (result.accepted) {
            f_->refusal->hide();
            return;
        }
        f_->refusal->setText(result.reason);
        f_->refusal->setVisible(!result.reason.isEmpty());
    };
    connect(f_->name, &QLineEdit::editingFinished, this, [this, report] {
        const auto* view = currentJob();
        if (refreshing_ || view == nullptr)
            return;
        const auto text = f_->name->text().toStdString();
        if (text != view->record.name)
            report(adapter_.editJob(view->id, jobEdit([&](auto& edit) { edit.name = text; })));
    });
    connect(f_->jobEnabled, &QAbstractButton::toggled, this, [this, report](const bool enabled) {
        if (!refreshing_ && job_)
            report(adapter_.editJob(*job_, jobEdit([&](auto& edit) { edit.enabled = enabled; })));
    });
    connect(f_->rangeKind, &kit::KDropdown::activated, this, [this] {
        const auto* view = currentJob();
        if (refreshing_ || view == nullptr)
            return;
        document::ExportRange range = view->record.render.range;
        range.kind = static_cast<document::ExportRangeKind>(f_->rangeKind->currentData().toInt());
        if (range.kind == document::ExportRangeKind::Custom ||
            range.kind == document::ExportRangeKind::Frame) {
            // Start from what the job renders now.
            range.first = view->resolvedRange ? view->resolvedRange->first : range.first;
            range.last = view->resolvedRange ? view->resolvedRange->last : std::max(range.first, range.last);
        }
        const auto result = adapter_.editJob(view->id, jobEdit([&](auto& edit) { edit.range = range; }));
        f_->refusal->setVisible(!result.accepted && !result.reason.isEmpty());
        f_->refusal->setText(result.reason);
    });
    for (auto* field : {f_->first, f_->last})
        connect(field, &kit::KValueField::editFinished, this, [this] { commitRange(); });
    connect(f_->label, &kit::KColorChip::editFinished, this, [this] {
        if (refreshing_ || !job_)
            return;
        const auto chosen = f_->label->color().toQColor();
        const auto* view = currentJob();
        if (view != nullptr && view->record.labelColor == rgbOf(chosen))
            return;
        (void)adapter_.editJob(*job_, jobEdit([&](auto& edit) { edit.labelColor = std::optional<std::array<std::uint8_t, 3>>(rgbOf(chosen)); }));
    });
    connect(f_->clearLabel, &kit::KButton::clicked, this, [this] {
        if (job_)
            (void)adapter_.editJob(*job_, jobEdit([&](auto& edit) { edit.labelColor = std::optional<std::array<std::uint8_t, 3>>(); }));
    });
}

void ExportDetailPane::commitRange() {
    const auto* view = currentJob();
    if (refreshing_ || view == nullptr)
        return;
    document::ExportRange range = view->record.render.range;
    range.first = static_cast<std::uint64_t>(f_->first->integerValue());
    range.last = range.kind == document::ExportRangeKind::Frame
                     ? range.first
                     : static_cast<std::uint64_t>(f_->last->integerValue());
    if (range.kind == document::ExportRangeKind::Custom && range.last < range.first) {
        f_->rangeReason->setText(tr("The last frame comes before the first frame."));
        f_->rangeReason->show();
        return;
    }
    const auto result = adapter_.editJob(view->id, jobEdit([&](auto& edit) { edit.range = range; }));
    f_->refusal->setVisible(!result.accepted && !result.reason.isEmpty());
    f_->refusal->setText(result.reason);
}

const host::ExportJobView* ExportDetailPane::currentJob() const {
    if (!snapshot_ || !job_)
        return nullptr;
    for (const auto& job : snapshot_->jobs)
        if (job.id == *job_)
            return &job;
    return nullptr;
}

const host::ExportOutputView* ExportDetailPane::currentOutput() const {
    const auto* job = currentJob();
    if (job == nullptr || job->outputs.empty())
        return nullptr;
    for (const auto& output : job->outputs)
        if (output_ && output.id == *output_)
            return &output;
    return &job->outputs.front();
}

void ExportDetailPane::setSelection(const std::optional<document::ExportJobId> job,
                                    const std::optional<document::ExportOutputId> output) {
    const bool changedJob = job != job_;
    job_ = job;
    output_ = output;
    if (changedJob)
        f_->refusal->hide();
    refresh();
}

void ExportDetailPane::refresh() {
    snapshot_ = adapter_.snapshot();
    catalogue_ = adapter_.catalogue();
    const auto* job = currentJob();
    setVisible(job != nullptr);
    if (job == nullptr)
        return;
    // The first output stands in when a job row is selected.
    const auto* output = currentOutput();
    output_ = output ? std::optional(output->id) : std::nullopt;
    refreshing_ = true;
    refreshJobActions(*job);
    refreshRender(*job);
    refreshOutput(*job, output);
    refreshResults(*job, output);
    refreshing_ = false;
    refreshLog(*job);
}

void ExportDetailPane::refreshJobActions(const host::ExportJobView& job) {
    const auto set = [](kit::KButton& button, const host::ExportCommandAvailability& availability) {
        button.setEnabled(availability.available);
        button.setToolTip(availability.available ? QString{} : qs(availability.reason));
    };
    set(*f_->start, job.commands[host::ExportCommand::Start]);
    set(*f_->stop, job.commands[host::ExportCommand::Stop]);
    set(*f_->retry, job.commands[host::ExportCommand::Retry]);
    const bool paused = job.state == host::ExportJobState::Paused;
    f_->pauseResume->setText(paused ? tr("Resume") : tr("Pause"));
    set(*f_->pauseResume, job.commands[paused ? host::ExportCommand::Resume
                                              : host::ExportCommand::Pause]);

}

void ExportDetailPane::refreshRender(const host::ExportJobView& job) {
    const auto& edit = job.commands[host::ExportCommand::EditJob];
    for (auto* widget : f_->editors) {
        widget->setEnabled(edit.available);
        widget->setToolTip(edit.available ? widget->toolTip() : qs(edit.reason));
    }
    const auto* composition = session_.snapshot().project().findComposition(job.record.composition);
    f_->name->setText(qs(job.record.name));
    f_->name->setPlaceholderText(composition ? qs(composition->name()) : tr("Missing composition"));
    {
        const QSignalBlocker block(f_->rangeKind);
        f_->rangeKind->setCurrentIndex(
            std::max(0, f_->rangeKind->findData(static_cast<int>(job.record.render.range.kind))));
    }
    const auto kind = job.record.render.range.kind;
    const bool custom = kind == document::ExportRangeKind::Custom;
    const bool frame = kind == document::ExportRangeKind::Frame;
    f_->first->setIntegerValue(static_cast<std::int64_t>(job.record.render.range.first));
    f_->last->setIntegerValue(static_cast<std::int64_t>(job.record.render.range.last));
    f_->firstRow->setVisible(custom || frame);
    f_->lastRow->setVisible(custom);
    f_->rangeSpan->setText(composition ? rangeTimecodeText(job, composition->format().frameRate())
                                       : QString{});
    f_->rangeSpanRow->setVisible(!f_->rangeSpan->text().isEmpty());
    // Job-level problems the host found (missing composition, range outside it, no enabled output).
    QStringList reasons;
    for (const auto& issue : orderedIssues(job.preflight))
        if (!reasons.contains(qs(issue.reason)))
            reasons.push_back(qs(issue.reason));
    f_->rangeReason->setText(reasons.join(QLatin1Char(' ')));
    f_->rangeReason->setVisible(!reasons.isEmpty());
    f_->jobEnabled->setChecked(job.record.enabled);
    if (job.record.labelColor.has_value()) {
        const auto& rgb = *job.record.labelColor;
        f_->label->setColor(kit::KColor::fromQColor(QColor(rgb[0], rgb[1], rgb[2])));
    } else {
        // No label: a fully transparent chip reads as the empty state, as a layer's does.
        f_->label->setColor(kit::KColor::fromRgba(0.0F, 0.0F, 0.0F, 0.0F));
    }
    f_->clearLabel->setEnabled(edit.available && job.record.labelColor.has_value());
}


} // namespace bloom::ui::exportpanel
