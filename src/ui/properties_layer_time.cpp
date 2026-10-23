#include "properties_layer_time.hpp"

#include "properties_sections.hpp"

#include <QSignalBlocker>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <cmath>
#include <optional>

namespace bloom::ui::properties {
namespace {

std::optional<document::LayerId> layerInContext(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary))
        return *direct;
    return session.selection().contextualLayer;
}

// Frame index n <-> exact time n / rate. Times off the grid show their nearest frame.
double frameOf(const core::RationalTime time, const document::FrameRate rate) {
    return std::round(time.toSeconds() * rate.numerator() / rate.denominator());
}
std::optional<core::RationalTime> timeOf(const double frame, const document::FrameRate rate) {
    if (!std::isfinite(frame) || std::abs(frame) > 1.0e12)
        return std::nullopt;
    const auto index = static_cast<std::int64_t>(std::llround(frame));
    return core::RationalTime::create(index * static_cast<std::int64_t>(rate.denominator()),
                                      rate.numerator());
}

struct LayerTimeFields final {
    kit::KSection* section = nullptr;
    kit::KValueField* start = nullptr;
    kit::KValueField* in = nullptr;
    kit::KValueField* out = nullptr;
    kit::KValueField* frames = nullptr;
};

kit::KValueField* frameCell(const QString& name, const QString& accessible, QWidget* parent) {
    return makeValueCell({.objectName = name,
                          .accessibleName = accessible,
                          .subLabel = {},
                          .minimum = -1.0e9,
                          .maximum = 1.0e9,
                          .decimals = 0,
                          .singleStep = 1.0,
                          .unit = QStringLiteral("f")},
                         parent);
}

void refresh(CompositionSession& session, const LayerTimeFields& fields) {
    const auto* composition = session.composition();
    const auto layerId = layerInContext(session);
    const auto* layer = composition && layerId ? composition->graph().findLayer(*layerId) : nullptr;
    fields.section->setVisible(layer != nullptr);
    if (layer == nullptr)
        return;
    const auto rate = composition->format().frameRate();
    const auto in = frameOf(layer->inPoint, rate);
    const auto out = frameOf(layer->endPoint(composition->duration()), rate);
    const std::pair<kit::KValueField*, double> values[]{
        {fields.start, frameOf(layer->timing.startTime, rate)},
        {fields.in, in},
        {fields.out, out},
        {fields.frames, out - in}};
    for (const auto& [field, value] : values) {
        const QSignalBlocker blocker(field);
        field->setValue(value);
        field->setEnabled(!layer->locked);
    }
}

// Commits the four fields as one SetLayerTiming. `moved` shifts in/out with the start.
void commit(CompositionSession& session, const LayerTimeFields& fields, const int edited) {
    const auto* composition = session.composition();
    const auto layerId = layerInContext(session);
    const auto* layer = composition && layerId ? composition->graph().findLayer(*layerId) : nullptr;
    if (layer == nullptr)
        return;
    const auto rate = composition->format().frameRate();
    double start = frameOf(layer->timing.startTime, rate);
    double in = frameOf(layer->inPoint, rate);
    double out = frameOf(layer->endPoint(composition->duration()), rate);
    switch (edited) {
    case 0: { // Start moves the whole layer: content, keys and range together.
        const auto delta = fields.start->value() - start;
        start += delta;
        in += delta;
        out += delta;
        break;
    }
    case 1:
        in = fields.in->value();
        break;
    case 2:
        out = fields.out->value();
        break;
    default:
        out = in + fields.frames->value();
        break;
    }
    // A start time between frames keeps its exact offset unless the start itself was edited.
    auto startTime = edited == 0 ? timeOf(start, rate) : std::optional{layer->timing.startTime};
    const auto inTime = timeOf(in, rate);
    const auto outTime = timeOf(out, rate);
    if (!startTime || !inTime || !outTime) {
        refresh(session, fields);
        return;
    }
    commands::Transaction transaction(edited == 0 ? "Move Layer" : "Set Layer Range",
                                      session.snapshot().revision());
    transaction.emplace<commands::SetLayerTiming>(session.compositionId(), *layerId, *startTime,
                                                  *inTime, *outTime, layer->timing.speed);
    (void)session.executeTransaction(std::move(transaction));
    // A refused edit (for example a range wholly outside the composition) restores the fields.
    refresh(session, fields);
}

} // namespace

void addLayerTimeSection(CompositionSession& session, QVBoxLayout* layout, QWidget* parent) {
    LayerTimeFields fields;
    fields.section = addSection(layout, parent, QStringLiteral("layer-time"), QObject::tr("Time"));
    auto* body = fields.section->body();
    auto* rows = fields.section->bodyLayout();
    fields.start = frameCell(QStringLiteral("layerStartFrameEditor"), QObject::tr("Start frame"), body);
    fields.in = frameCell(QStringLiteral("layerInFrameEditor"), QObject::tr("In frame"), body);
    fields.out = frameCell(QStringLiteral("layerOutFrameEditor"), QObject::tr("Out frame"), body);
    fields.frames = frameCell(QStringLiteral("layerFrameCountEditor"), QObject::tr("Frames"), body);
    (void)addRow(rows, body, makeRowLabel(QObject::tr("Start"), body), nullptr, fields.start);
    (void)addRow(rows, body, makeRowLabel(QObject::tr("In"), body), nullptr, fields.in);
    (void)addRow(rows, body, makeRowLabel(QObject::tr("Out"), body), nullptr, fields.out);
    (void)addRow(rows, body, makeRowLabel(QObject::tr("Frames"), body), nullptr, fields.frames);
    const std::array cells{fields.start, fields.in, fields.out, fields.frames};
    for (int index = 0; index < static_cast<int>(cells.size()); ++index)
        QObject::connect(cells[static_cast<std::size_t>(index)], &kit::KValueField::editFinished,
                         fields.section, [&session, fields, index] {
                             commit(session, fields, index);
                         });
    const auto update = [&session, fields] { refresh(session, fields); };
    QObject::connect(&session, &CompositionSession::snapshotChanged, fields.section, update);
    QObject::connect(&session, &CompositionSession::selectionChanged, fields.section, update);
    refresh(session, fields);
}

} // namespace bloom::ui::properties
