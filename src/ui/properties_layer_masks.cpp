#include "properties_layer_masks.hpp"

#include "layer_mask_support.hpp"
#include "properties_sections.hpp"
#include "properties_value_edits.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <algorithm>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::ui::properties {
namespace {

std::optional<document::LayerId> layerInContext(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary))
        return *direct;
    return session.selection().contextualLayer;
}

const document::LayerOutputBoundary* contextLayer(const CompositionSession& session) {
    const auto* composition = session.composition();
    const auto id = layerInContext(session);
    return composition && id ? composition->graph().findLayer(*id) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Masks
// ---------------------------------------------------------------------------------------------

// Runs `select` on a press or focus-in anywhere in the group; never consumes the event.
class SelectOnPress final : public QObject {
  public:
    SelectOnPress(QObject* parent, std::function<void()> select)
        : QObject(parent), select_(std::move(select)) {}

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn)
            select_();
        return false;
    }

  private:
    std::function<void()> select_;
};

struct MaskGroup final {
    document::LayerMaskId id;
    QWidget* box = nullptr;
    kit::KLineEdit* name = nullptr;
    kit::KButton* up = nullptr;
    kit::KButton* down = nullptr;
    kit::KButton* remove = nullptr;
    kit::KDropdown* mode = nullptr;
    kit::KSwitch* inverted = nullptr;
    kit::KLabel* pathSummary = nullptr;
    KeyframeDiamond* pathDiamond = nullptr;
    struct Scalar final {
        kit::KValueField* field = nullptr;
        KeyframeDiamond* diamond = nullptr;
    };
    Scalar feather, expansion, opacity;
};

struct MasksState final {
    kit::KSection* section = nullptr;
    std::optional<document::LayerId> layer;
    std::vector<document::LayerMaskId> ids;
    bool locked = false;
    std::vector<MaskGroup> groups;
};

void clearBody(MasksState& state) {
    auto* layout = state.section->bodyLayout();
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    state.groups.clear();
}

template <typename Command, typename... Args>
void runMaskCommand(CompositionSession& session, const char* label, document::LayerId layer,
                    Args&&... args) {
    commands::Transaction transaction(label, session.snapshot().revision());
    transaction.emplace<Command>(session.compositionId(), layer, std::forward<Args>(args)...);
    (void)session.executeTransaction(std::move(transaction));
}

MaskGroup::Scalar makeScalar(CompositionSession& session, MasksState& state, QWidget* box,
                             QVBoxLayout* rows, const QString& label, const QString& objectName,
                             const document::ParameterId parameter, const double minimum,
                             const double maximum, const double step, const QString& unit,
                             const QString& commandLabel) {
    MaskGroup::Scalar scalar;
    scalar.field = makeValueCell({.objectName = objectName,
                                  .accessibleName = label,
                                  .subLabel = {},
                                  .minimum = minimum,
                                  .maximum = maximum,
                                  .decimals = 2,
                                  .singleStep = step,
                                  .unit = unit},
                                 box);
    scalar.diamond = makeKeyframeDiamond(session, "mask", box);
    scalar.diamond->setParameterId(parameter);
    (void)addRow(rows, box, makeRowLabel(label, box), scalar.diamond, scalar.field);
    bindValueEdit(session, *scalar.field, [parameter] { return parameter; });
    auto* field = scalar.field;
    QObject::connect(field, &kit::KValueField::valueChanged, field,
                     [&session, parameter, commandLabel](const double value) {
                         (void)session.setParameterValue(parameter, value, commandLabel);
                     });
    (void)state;
    return scalar;
}

void refreshGroup(CompositionSession& session, const document::LayerOutputBoundary& layer,
                  const document::LayerMask& mask, MaskGroup& group, const bool selected) {
    const auto* composition = session.composition();
    if (!group.name->hasFocus()) {
        const QSignalBlocker blocker(group.name);
        group.name->setText(QString::fromStdString(mask.name));
    }
    {
        const QSignalBlocker blocker(group.mode);
        group.mode->setCurrentIndex(group.mode->findData(static_cast<int>(mask.mode)));
    }
    {
        const QSignalBlocker blocker(group.inverted);
        group.inverted->setChecked(mask.inverted);
    }
    const auto keyTime = session.keyTimeAtPlayhead(mask.path);
    if (const auto path = masks::maskPathAt(*composition, mask, keyTime))
        group.pathSummary->setText(
            QObject::tr("%1 anchors · %2")
                .arg(path->anchors.size())
                .arg(path->closed ? QObject::tr("Closed") : QObject::tr("Open")));
    const std::pair<MaskGroup::Scalar&, document::ParameterId> scalars[]{
        {group.feather, mask.feather},
        {group.expansion, mask.expansion},
        {group.opacity, mask.opacity}};
    for (auto& [scalar, parameter] : scalars) {
        if (const auto value = session.effectiveScalarValue(parameter);
            value && !session.isValueEditing(parameter)) {
            const QSignalBlocker blocker(scalar.field);
            scalar.field->setValue(*value);
        }
        scalar.diamond->refresh();
        scalar.field->setEnabled(!layer.locked);
    }
    group.pathDiamond->refresh();
    group.name->setEnabled(!layer.locked);
    group.mode->setEnabled(!layer.locked);
    group.inverted->setEnabled(!layer.locked);
    group.remove->setEnabled(!layer.locked);
    group.up->setEnabled(!layer.locked && mask.id != layer.masks.front().id);
    group.down->setEnabled(!layer.locked && mask.id != layer.masks.back().id);
    group.box->setProperty("selectedMask", selected);
}

void refreshValues(CompositionSession& session, MasksState& state) {
    const auto* layer = contextLayer(session);
    if (layer == nullptr || state.groups.size() != layer->masks.size())
        return;
    const auto selected = session.selectedMask();
    for (std::size_t i = 0; i < state.groups.size(); ++i)
        refreshGroup(session, *layer, layer->masks[i], state.groups[i],
                     selected && selected->mask == layer->masks[i].id);
}

void buildGroup(CompositionSession& session, MasksState& state, const document::LayerId layerId,
                const document::LayerMask& mask, const std::size_t number) {
    auto* body = state.section->body();
    auto* box = new QWidget(body);
    box->setObjectName(QStringLiteral("layerMaskGroup"));
    box->setProperty("maskId", QVariant::fromValue(static_cast<qulonglong>(mask.id.value())));
    auto* rows = new QVBoxLayout(box);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kit::px(kit::Spacing::Gutter));
    state.section->bodyLayout()->addWidget(box);

    MaskGroup group;
    group.id = mask.id;
    group.box = box;

    auto* header = new QWidget(box);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(kit::px(kit::Spacing::XS));
    group.name = new kit::KLineEdit(header);
    group.name->setObjectName(QStringLiteral("layerMaskNameEdit"));
    group.name->setAccessibleName(QObject::tr("Mask %1 name").arg(number));
    group.name->setFont(kit::font(kit::TypeRole::Value));
    group.name->setFixedHeight(kit::px(kit::Size::Control));
    headerLayout->addWidget(group.name, 1);
    const auto button = [&](const QString& name, const QString& tip, const kit::IconId icon) {
        auto* b = new kit::KButton(header);
        b->setObjectName(name);
        b->setIconId(icon);
        b->setVariant(kit::KButton::Variant::Ghost);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
        headerLayout->addWidget(b);
        return b;
    };
    group.up = button(QStringLiteral("layerMaskMoveUpButton"), QObject::tr("Move mask up"),
                      kit::IconId::CaretUp);
    group.down = button(QStringLiteral("layerMaskMoveDownButton"), QObject::tr("Move mask down"),
                        kit::IconId::CaretDown);
    group.remove = button(QStringLiteral("layerMaskRemoveButton"), QObject::tr("Remove mask"),
                          kit::IconId::Delete);
    rows->addWidget(header);

    group.mode = new kit::KDropdown(box);
    group.mode->setObjectName(QStringLiteral("layerMaskModeDropdown"));
    group.mode->setAccessibleName(QObject::tr("Mask mode"));
    for (const auto m : {document::LayerMaskMode::None, document::LayerMaskMode::Add,
                         document::LayerMaskMode::Subtract, document::LayerMaskMode::Intersect,
                         document::LayerMaskMode::Difference})
        group.mode->addItem(masks::modeText(m), QVariant::fromValue(static_cast<int>(m)));
    (void)addRow(rows, box, makeRowLabel(QObject::tr("Mode"), box), nullptr, group.mode);
    group.inverted = new kit::KSwitch(box);
    group.inverted->setObjectName(QStringLiteral("layerMaskInvertedSwitch"));
    group.inverted->setAccessibleName(QObject::tr("Inverted"));
    (void)addRow(rows, box, makeRowLabel(QObject::tr("Inverted"), box), nullptr, group.inverted);

    group.pathSummary = new kit::KLabel(box);
    group.pathSummary->setObjectName(QStringLiteral("layerMaskPathSummary"));
    group.pathDiamond = makeKeyframeDiamond(session, "mask", box);
    group.pathDiamond->setParameterId(mask.path);
    (void)addRow(rows, box, makeRowLabel(QObject::tr("Path"), box), group.pathDiamond,
                 group.pathSummary);
    group.feather = makeScalar(session, state, box, rows, QObject::tr("Feather"),
                               QStringLiteral("layerMaskFeatherField"), mask.feather, 0.0, 1000.0,
                               1.0, QStringLiteral("px"), QObject::tr("Set Mask Feather"));
    group.expansion =
        makeScalar(session, state, box, rows, QObject::tr("Expansion"),
                   QStringLiteral("layerMaskExpansionField"), mask.expansion, -1000.0, 1000.0, 1.0,
                   QStringLiteral("px"), QObject::tr("Set Mask Expansion"));
    group.opacity = makeScalar(session, state, box, rows, QObject::tr("Opacity"),
                               QStringLiteral("layerMaskOpacityField"), mask.opacity, 0.0, 1.0,
                               0.05, {}, QObject::tr("Set Mask Opacity"));

    const auto maskId = mask.id;
    QObject::connect(group.name, &QLineEdit::editingFinished, group.name,
                     [&session, layerId, maskId, edit = group.name] {
                         runMaskCommand<commands::RenameLayerMask>(
                             session, "Rename Mask", layerId, maskId, edit->text().toStdString());
                     });
    QObject::connect(group.mode, &kit::KDropdown::activated, group.mode,
                     [&session, layerId, maskId, dropdown = group.mode](int) {
                         runMaskCommand<commands::SetLayerMaskMode>(
                             session, "Set Mask Mode", layerId, maskId,
                             static_cast<document::LayerMaskMode>(dropdown->currentData().toInt()));
                     });
    QObject::connect(group.inverted, &kit::KSwitch::toggled, group.inverted,
                     [&session, layerId, maskId](const bool on) {
                         runMaskCommand<commands::SetLayerMaskInverted>(session, "Invert Mask",
                                                                        layerId, maskId, on);
                     });
    QObject::connect(group.up, &QAbstractButton::clicked, group.up, [&session, layerId, maskId] {
        const auto* layer = session.composition()->graph().findLayer(layerId);
        const auto at = std::ranges::find(layer->masks, maskId, &document::LayerMask::id);
        if (layer != nullptr && at != layer->masks.end() && at != layer->masks.begin())
            runMaskCommand<commands::MoveLayerMask>(
                session, "Move Mask", layerId, maskId,
                static_cast<std::size_t>(std::distance(layer->masks.begin(), at)) - 1);
    });
    QObject::connect(
        group.down, &QAbstractButton::clicked, group.down, [&session, layerId, maskId] {
            const auto* layer = session.composition()->graph().findLayer(layerId);
            const auto at = std::ranges::find(layer->masks, maskId, &document::LayerMask::id);
            if (layer != nullptr && at != layer->masks.end() && std::next(at) != layer->masks.end())
                runMaskCommand<commands::MoveLayerMask>(
                    session, "Move Mask", layerId, maskId,
                    static_cast<std::size_t>(std::distance(layer->masks.begin(), at)) + 1);
        });
    QObject::connect(
        group.remove, &QAbstractButton::clicked, group.remove, [&session, layerId, maskId] {
            runMaskCommand<commands::RemoveLayerMask>(session, "Remove Mask", layerId, maskId);
        });
    // Touching anything in the group selects its mask for the Viewer.
    auto* filter = new SelectOnPress(box, [&session, layerId, maskId] {
        const auto current = session.selectedMask();
        if (!current || current->mask != maskId)
            session.selectMask(layerId, maskId);
    });
    box->installEventFilter(filter);
    for (auto* child : box->findChildren<QWidget*>())
        child->installEventFilter(filter);
    state.groups.push_back(group);
}

void rebuildMasks(CompositionSession& session, const std::shared_ptr<MasksState>& state) {
    const auto* layer = contextLayer(session);
    state->section->setVisible(layer != nullptr);
    if (layer == nullptr) {
        if (state->layer) {
            clearBody(*state);
            state->layer.reset();
            state->ids.clear();
        }
        return;
    }
    std::vector<document::LayerMaskId> ids;
    for (const auto& mask : layer->masks)
        ids.push_back(mask.id);
    if (state->layer != layer->layerId || state->ids != ids) {
        clearBody(*state);
        state->layer = layer->layerId;
        state->ids = ids;
        std::size_t number = 0;
        for (const auto& mask : layer->masks)
            buildGroup(session, *state, layer->layerId, mask, ++number);
    }
    refreshValues(session, *state);
}

// ---------------------------------------------------------------------------------------------
// Track matte
// ---------------------------------------------------------------------------------------------

struct MatteFields final {
    kit::KSection* section = nullptr;
    kit::KDropdown* source = nullptr;
    kit::KDropdown* channel = nullptr;
    kit::KSwitch* inverted = nullptr;
};

void refreshMatte(CompositionSession& session, const MatteFields& fields) {
    const auto* layer = contextLayer(session);
    fields.section->setVisible(layer != nullptr);
    if (layer == nullptr)
        return;
    const auto* composition = session.composition();
    const auto candidates = masks::matteSourceCandidates(*composition, layer->layerId);
    const QSignalBlocker sourceBlocker(fields.source);
    fields.source->clearItems();
    fields.source->addItem(QObject::tr("None"), QVariant::fromValue(qulonglong{0}));
    for (const auto id : candidates)
        if (const auto* other = composition->graph().findLayer(id))
            fields.source->addItem(QString::fromStdString(other->name),
                                   QVariant::fromValue(static_cast<qulonglong>(id.value())));
    const auto matte = layer->trackMatte;
    fields.source->setCurrentIndex(fields.source->findData(
        QVariant::fromValue(static_cast<qulonglong>(matte ? matte->source.value() : 0))));
    {
        const QSignalBlocker blocker(fields.channel);
        fields.channel->setCurrentIndex(fields.channel->findData(
            static_cast<int>(matte ? matte->channel : document::TrackMatteChannel::Alpha)));
    }
    {
        const QSignalBlocker blocker(fields.inverted);
        fields.inverted->setChecked(matte && matte->inverted);
    }
    fields.source->setEnabled(!layer->locked);
    fields.channel->setEnabled(!layer->locked && matte.has_value());
    fields.inverted->setEnabled(!layer->locked && matte.has_value());
}

void commitMatte(CompositionSession& session, const MatteFields& fields) {
    const auto* layer = contextLayer(session);
    if (layer == nullptr)
        return;
    const auto source = fields.source->currentData().toULongLong();
    std::optional<document::TrackMatte> matte;
    if (source != 0)
        matte = document::TrackMatte{
            document::LayerId::fromRaw(source),
            static_cast<document::TrackMatteChannel>(fields.channel->currentData().toInt()),
            fields.inverted->isChecked()};
    (void)session.setTrackMatte(layer->layerId, matte);
    refreshMatte(session, fields);
}

} // namespace

void addLayerMasksSection(CompositionSession& session, QVBoxLayout* layout, QWidget* parent) {
    auto state = std::make_shared<MasksState>();
    state->section =
        addSection(layout, parent, QStringLiteral("layer-masks"), QObject::tr("Masks"));
    state->section->setResetEnabled(false);
    const auto update = [&session, state] { rebuildMasks(session, state); };
    QObject::connect(&session, &CompositionSession::snapshotChanged, state->section, update);
    QObject::connect(&session, &CompositionSession::selectionChanged, state->section, update);
    QObject::connect(&session, &CompositionSession::maskSelectionChanged, state->section, update);
    QObject::connect(&session, &CompositionSession::currentTimeChanged, state->section, update);
    QObject::connect(&session, &CompositionSession::liveValueChanged, state->section,
                     [&session, state] { refreshValues(session, *state); });
    rebuildMasks(session, state);
}

void addTrackMatteSection(CompositionSession& session, QVBoxLayout* layout, QWidget* parent) {
    MatteFields fields;
    fields.section =
        addSection(layout, parent, QStringLiteral("track-matte"), QObject::tr("Track Matte"));
    fields.section->setResetEnabled(false);
    auto* body = fields.section->body();
    auto* rows = fields.section->bodyLayout();
    fields.source = new kit::KDropdown(body);
    fields.source->setObjectName(QStringLiteral("layerTrackMatteSourceDropdown"));
    fields.source->setAccessibleName(QObject::tr("Matte layer"));
    fields.channel = new kit::KDropdown(body);
    fields.channel->setObjectName(QStringLiteral("layerTrackMatteChannelDropdown"));
    fields.channel->setAccessibleName(QObject::tr("Matte channel"));
    for (const auto channel :
         {document::TrackMatteChannel::Alpha, document::TrackMatteChannel::Luma})
        fields.channel->addItem(masks::channelText(channel),
                                QVariant::fromValue(static_cast<int>(channel)));
    fields.inverted = new kit::KSwitch(body);
    fields.inverted->setObjectName(QStringLiteral("layerTrackMatteInvertedSwitch"));
    fields.inverted->setAccessibleName(QObject::tr("Invert matte"));
    (void)addRow(rows, body, makeRowLabel(QObject::tr("Layer"), body), nullptr, fields.source);
    (void)addRow(rows, body, makeRowLabel(QObject::tr("Channel"), body), nullptr, fields.channel);
    (void)addRow(rows, body, makeRowLabel(QObject::tr("Inverted"), body), nullptr, fields.inverted);
    QObject::connect(fields.source, &kit::KDropdown::activated, fields.section,
                     [&session, fields](int) { commitMatte(session, fields); });
    QObject::connect(fields.channel, &kit::KDropdown::activated, fields.section,
                     [&session, fields](int) { commitMatte(session, fields); });
    QObject::connect(fields.inverted, &kit::KSwitch::toggled, fields.section,
                     [&session, fields](bool) { commitMatte(session, fields); });
    const auto update = [&session, fields] { refreshMatte(session, fields); };
    QObject::connect(&session, &CompositionSession::snapshotChanged, fields.section, update);
    QObject::connect(&session, &CompositionSession::selectionChanged, fields.section, update);
    refreshMatte(session, fields);
}

} // namespace bloom::ui::properties
