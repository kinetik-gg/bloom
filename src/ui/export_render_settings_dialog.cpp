#include "export_render_settings_dialog.hpp"

#include "assets_editor_internal.hpp"
#include "export_panel_text.hpp"

#include <bloom/document/project.hpp>
#include <bloom/host/export_job_runner.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::exportpanel {
namespace {

using document::ExportRangeKind;

// A field that keeps its own width beside what follows it; the row's slack stays empty.
[[nodiscard]] QWidget* fieldRow(QWidget* parent, std::initializer_list<QWidget*> widgets) {
    auto* holder = new QWidget(parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::S));
    for (auto* widget : widgets)
        layout->addWidget(widget);
    layout->addStretch(1);
    return holder;
}

} // namespace

RenderSettingsDialog::RenderSettingsDialog(const document::Project& project,
                                           const host::ExportJobView& job, QWidget& owner)
    : kit::KDialog(tr("Render Settings"), QStringLiteral("exportRenderSettingsDialog"), owner,
                   Qt::WindowModal, Width::Wide),
      initial_(job.record.render.range) {
    const auto* composition = project.findComposition(job.record.composition);
    if (composition != nullptr)
        rate_ = composition->format().frameRate();
    auto probe = job.record;
    probe.render.range = {ExportRangeKind::WorkArea, 0, 0};
    workArea_ = host::resolveExportRange(probe, project);
    probe.render.range = {ExportRangeKind::Composition, 0, 0};
    whole_ = host::resolveExportRange(probe, project);

    const auto name = composition != nullptr ? qs(composition->name()) : qs(job.displayName);
    auto* section = new kit::KSection(tr("Composition “%1”").arg(name), this);
    section->setObjectName(QStringLiteral("exportRenderSettingsComposition"));
    section->setResetEnabled(false);
    contentLayout()->addWidget(section);
    auto* columns = new QWidget(section->body());
    auto* columnsLayout = new QHBoxLayout(columns);
    columnsLayout->setContentsMargins(0, 0, 0, 0);
    columnsLayout->setSpacing(kit::px(kit::Spacing::XL));
    section->bodyLayout()->addWidget(columns);
    const auto column = [this, columns, columnsLayout] {
        auto* widget = new QWidget(columns);
        auto* layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(kit::px(kit::Spacing::S));
        auto* form = addForm(*layout, *widget);
        layout->addStretch(1);
        columnsLayout->addWidget(widget, 1);
        return form;
    };
    auto* left = column();
    times_ = column();

    kind_ = new kit::KDropdown(this);
    kind_->setObjectName(QStringLiteral("exportRangeKind"));
    kind_->setAccessibleName(tr("Range"));
    kind_->addItem(tr("Work area"), static_cast<int>(ExportRangeKind::WorkArea));
    kind_->addItem(tr("Whole composition"), static_cast<int>(ExportRangeKind::Composition));
    kind_->addItem(tr("Custom"), static_cast<int>(ExportRangeKind::Custom));
    kind_->addItem(tr("One frame"), static_cast<int>(ExportRangeKind::Frame));
    (void)left->addRow(tr("Range"), fieldRow(this, {kind_}));

    const auto frameField = [this](const QString& name, const QString& label) {
        auto* field = new kit::KValueField(this);
        field->setObjectName(name);
        field->setAccessibleName(label);
        field->setIntegerMode(true);
        field->setIntegerRange(0, 100'000'000);
        field->setUnit(tr("f"));
        field->setFixedWidth(kit::px(kit::Size::DropdownWidth));
        return field;
    };
    const auto timeLabel = [this](const QString& name) {
        auto* label = new kit::KLabel(this);
        label->setObjectName(name);
        label->setTone(kit::Color::Muted);
        return label;
    };
    first_ = frameField(QStringLiteral("exportRangeStart"), tr("Start"));
    last_ = frameField(QStringLiteral("exportRangeEnd"), tr("End"));
    firstTime_ = timeLabel(QStringLiteral("exportRangeStartTime"));
    lastTime_ = timeLabel(QStringLiteral("exportRangeEndTime"));
    duration_ = timeLabel(QStringLiteral("exportRangeDuration"));
    firstRow_ = fieldRow(this, {first_, firstTime_});
    lastRow_ = fieldRow(this, {last_, lastTime_});
    (void)times_->addRow(tr("Start"), firstRow_);
    (void)times_->addRow(tr("End"), lastRow_);
    (void)times_->addRow(tr("Duration"), fieldRow(this, {duration_}));

    // A custom range starts from what the job renders now.
    const auto now = resolved().value_or(host::ExportResolvedRange{initial_.first, initial_.last});
    kind_->setCurrentIndex(std::max(0, kind_->findData(static_cast<int>(initial_.kind))));
    first_->setIntegerValue(static_cast<std::int64_t>(
        initial_.kind == ExportRangeKind::Custom || initial_.kind == ExportRangeKind::Frame ? initial_.first
                                                                                            : now.first));
    last_->setIntegerValue(static_cast<std::int64_t>(
        initial_.kind == ExportRangeKind::Custom ? initial_.last : now.last));

    connect(kind_, &kit::KDropdown::currentIndexChanged, this, [this](int) {
        const auto kind = static_cast<ExportRangeKind>(kind_->currentData().toInt());
        if (kind == ExportRangeKind::Custom || kind == ExportRangeKind::Frame) {
            // Switching to a custom range or one frame keeps the frames on screen.
            const auto shown = resolved();
            if (kind == ExportRangeKind::Custom && shown.has_value() && last_->integerValue() < first_->integerValue())
                last_->setIntegerValue(static_cast<std::int64_t>(shown->last));
        }
        refresh();
    });
    for (auto* field : {first_, last_})
        connect(field, &kit::KValueField::integerValueChanged, this, [this](qint64) { refresh(); });

    buttonBox()->setObjectName(QStringLiteral("exportRenderSettingsButtons"));
    addButton(Role::Accept, tr("OK"))->setObjectName(QStringLiteral("exportRenderSettingsOk"));
    addButton(Role::Reject, tr("Cancel"))->setObjectName(QStringLiteral("exportRenderSettingsCancel"));
    setValidator([this]() -> std::optional<kit::KRefusal> {
        const auto range = this->range();
        const auto last = whole_.has_value() ? whole_->last : 0;
        if (range.kind == ExportRangeKind::Custom && range.last < range.first)
            return kit::KRefusal{last_, tr("The end comes before the start.")};
        if ((range.kind == ExportRangeKind::Custom || range.kind == ExportRangeKind::Frame) &&
            whole_.has_value() && (range.first > last || range.last > last))
            return kit::KRefusal{range.first > last ? first_ : last_,
                                 tr("The composition ends at frame %1.").arg(last)};
        return std::nullopt;
    });
    watch(kind_);
    watch(first_);
    watch(last_);
    setInitialFocus(kind_);
    refresh();
}

document::ExportRange RenderSettingsDialog::range() const {
    const auto kind = static_cast<ExportRangeKind>(kind_->currentData().toInt());
    switch (kind) {
    case ExportRangeKind::Custom:
        return {kind, static_cast<std::uint64_t>(first_->integerValue()),
                static_cast<std::uint64_t>(last_->integerValue())};
    case ExportRangeKind::Frame:
        return {kind, static_cast<std::uint64_t>(first_->integerValue()),
                static_cast<std::uint64_t>(first_->integerValue())};
    case ExportRangeKind::WorkArea:
    case ExportRangeKind::Composition:
        // The job's own frames are kept, so switching back to Custom finds them again.
        return {kind, initial_.first, initial_.last};
    }
    return initial_;
}

std::optional<host::ExportResolvedRange> RenderSettingsDialog::resolved() const {
    const auto range = this->range();
    switch (range.kind) {
    case ExportRangeKind::WorkArea:
        return workArea_;
    case ExportRangeKind::Composition:
        return whole_;
    case ExportRangeKind::Custom:
    case ExportRangeKind::Frame:
        if (range.last < range.first || (whole_.has_value() && range.last > whole_->last))
            return std::nullopt;
        return host::ExportResolvedRange{range.first, range.last};
    }
    return std::nullopt;
}

void RenderSettingsDialog::refresh() {
    const auto kind = static_cast<ExportRangeKind>(kind_->currentData().toInt());
    const bool editable = kind == ExportRangeKind::Custom || kind == ExportRangeKind::Frame;
    first_->setVisible(editable);
    last_->setVisible(kind == ExportRangeKind::Custom);
    times_->setRowVisible(lastRow_, kind != ExportRangeKind::Frame);
    times_->labelFor(firstRow_)->setText(kind == ExportRangeKind::Frame ? tr("Frame") : tr("Start"));
    const auto span = resolved();
    const auto frame = [this](const std::uint64_t index) {
        return tr("%1 · frame %2").arg(assets::timecodeForFrames(index, rate_)).arg(index);
    };
    const auto timecode = [this](const std::uint64_t index) {
        return assets::timecodeForFrames(index, rate_);
    };
    if (span.has_value()) {
        firstTime_->setText(editable ? timecode(span->first) : frame(span->first));
        lastTime_->setText(kind == ExportRangeKind::Custom ? timecode(span->last) : frame(span->last));
        const auto frames = span->last - span->first + 1;
        duration_->setText(frames == 1 ? tr("%1 · 1 frame").arg(timecode(frames))
                                       : tr("%1 · %2 frames").arg(timecode(frames)).arg(frames));
    } else {
        firstTime_->setText(editable ? QString{} : tr("Not set"));
        lastTime_->setText(kind == ExportRangeKind::Custom ? QString{} : tr("Not set"));
        duration_->setText(tr("Not set"));
    }
    revalidate();
}

QString RenderSettingsDialog::startText() const { return firstTime_->text(); }
QString RenderSettingsDialog::endText() const { return lastTime_->text(); }
QString RenderSettingsDialog::durationText() const { return duration_->text(); }
bool RenderSettingsDialog::endShown() const { return times_->isRowVisible(lastRow_); }

} // namespace bloom::ui::exportpanel
