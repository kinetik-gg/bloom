#include "timeline_property_rows.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include "properties_value_edits.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QStackedLayout>
#include <QVariant>
#include <algorithm>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <memory>

namespace bloom::ui {
namespace {
// The three component captions a multi-component value cell can carry, in authored order.
[[nodiscard]] QString componentLabel(const std::size_t index, const bool color = false) {
    if (color)
        return QStringLiteral("RGBA").mid(static_cast<qsizetype>(index), 1);
    const std::array<QString, 4> captions{QStringLiteral("X"), QStringLiteral("Y"),
                                          QStringLiteral("Z"), QString{}};
    return captions[index];
}

// task TL-FIX2. The nesting step between consecutive depths (group -> parameter -> component):
// Spacing::M, the existing gutter token nearest a typical single-level list indent -- small enough
// that a component row's shrunken label (see bind()) still comfortably fits its one-letter
// "X"/"R" caption.
constexpr int kPropertyIndentStep = kit::px(kit::Spacing::M);

// depth 1 (a group, directly under its layer) lands at TimelineEditor::propertyNameIndent() --
// already reserved for exactly this and, until this task, unused; depth 2 (a parameter) and depth 3
// (an expanded component) each add one more step. TimelineEditor::propertyNameIndent()'s own
// formula keeps this strictly to the right of the layer row's own name text, so the row's depth
// series -- layer, group, parameter, component -- indents by one consistent step throughout.
[[nodiscard]] int propertyRowIndent(const int depth) {
    return TimelineEditor::propertyNameIndent() + (depth - 1) * kPropertyIndentStep;
}

// The two nodes a layer's own rows already come from. Task DRIVE-1's upstream walk starts from
// exactly these, so the value nodes it reaches are the ones driving what the twirl-down shows.
[[nodiscard]] std::vector<document::NodeId> upstreamSeeds(const CompositionSession& session,
                                                          const document::LayerId layerId) {
    std::vector<document::NodeId> seeds;
    if (const auto boundary = session.boundaryNodeForLayer(layerId))
        seeds.push_back(*boundary);
    if (const auto source = session.directSourceNodeForLayer(layerId))
        seeds.push_back(*source);
    return seeds;
}
} // namespace

QString upstreamGroupKey(const document::NodeId nodeId) {
    return QStringLiteral("upstream-%1").arg(nodeId.value());
}

std::vector<TimelineLayerEntry>
timelinePropertyEntries(const CompositionSession& session,
                        const std::vector<TimelineLayerEntry>& layers,
                        const std::set<document::LayerId>& expanded,
                        const std::set<std::pair<document::LayerId, QString>>& collapsedGroups,
                        const std::set<document::ParameterId>& expandedParameters) {
    std::vector<TimelineLayerEntry> rows;
    const auto* composition = session.composition();
    if (!composition)
        return rows;
    for (auto layer : layers) {
        layer.expanded = !layer.imageNodeId.isValid() && expanded.contains(layer.layerId);
        rows.push_back(layer);
        if (!layer.expanded)
            continue;
        bool groupOpen = true;
        // `key` is what the collapse set remembers; `title` is what the row shows. They are the
        // same string for the three built-in groups and differ for an upstream one (task DRIVE-1),
        // whose title is a node's display name -- which two nodes may share -- while its key is the
        // node's own identity and therefore never collides.
        const auto group = [&](const QString& key, const QString& title = {}) {
            auto entry = layer;
            entry.rowKind = TimelineLayerEntry::Kind::Group;
            entry.depth = 1;
            entry.name = title.isEmpty() ? key : title;
            entry.group = key;
            groupOpen = !collapsedGroups.contains({layer.layerId, key});
            entry.expanded = groupOpen;
            rows.push_back(entry);
        };
        const auto add = [&](document::NodeId nodeId, std::string_view role, const QString& name) {
            if (!groupOpen)
                return;
            const auto* node = composition->graph().findNode(nodeId);
            if (!node)
                return;
            for (const auto& binding : node->parameters) {
                if (binding.role != role)
                    continue;
                auto entry = layer;
                entry.rowKind = TimelineLayerEntry::Kind::Parameter;
                entry.depth = 2;
                entry.name = name;
                entry.role = binding.role;
                entry.parameterId = binding.parameterId;
                const auto* parameter = composition->parameters().find(binding.parameterId);
                const bool color =
                    parameter && document::isColor4AnimatableSchemaKey(parameter->schemaKey);
                const int count =
                    color                                                                    ? 4
                    : parameter && document::isVec3AnimatableSchemaKey(parameter->schemaKey) ? 3
                    : parameter && document::isVec2AnimatableSchemaKey(parameter->schemaKey) ? 2
                                                                                             : 0;
                entry.expanded = count > 0 && expandedParameters.contains(binding.parameterId);
                rows.push_back(entry);
                if (entry.expanded && session.driverBindingFor(binding.parameterId) == nullptr) {
                    const std::array vectorNames{document::AnimationComponent::X,
                                                 document::AnimationComponent::Y,
                                                 document::AnimationComponent::Z};
                    const std::array colorNames{
                        document::AnimationComponent::Red, document::AnimationComponent::Green,
                        document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
                    for (int index = 0; index < count; ++index) {
                        auto child = entry;
                        child.rowKind = TimelineLayerEntry::Kind::Component;
                        child.depth = 3;
                        child.component = color ? colorNames[static_cast<std::size_t>(index)]
                                                : vectorNames[static_cast<std::size_t>(index)];
                        child.name = componentLabel(static_cast<std::size_t>(index), color);
                        child.expanded = false;
                        rows.push_back(child);
                    }
                }
            }
        };
        if (const auto boundary = session.boundaryNodeForLayer(layer.layerId)) {
            group(QObject::tr("Object"));
            add(*boundary, document::kOpacityParameterRole, QObject::tr("Opacity"));
            add(*boundary, document::kBlendModeParameterRole, QObject::tr("Blending"));
            group(QObject::tr("Transform"));
            add(*boundary, document::kPositionParameterRole, QObject::tr("Position"));
            add(*boundary, document::kAnchorParameterRole, QObject::tr("Anchor"));
            add(*boundary, document::kScaleParameterRole, QObject::tr("Scale"));
            add(*boundary, document::kRotationParameterRole, QObject::tr("Rotation"));
        }
        if (const auto source = session.directSourceNodeForLayer(layer.layerId)) {
            const auto* node = composition->graph().findNode(*source);
            bool heading = false;
            const auto* definition =
                document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
            if (!definition)
                continue;
            for (const auto& declared : definition->parameters) {
                if (node->typeId == document::kShapeSourceNodeType &&
                    !document::shapeRoleVisible(nodeShapeKind(*composition, *node), declared.role))
                    continue;
                const auto found = std::ranges::find(node->parameters, declared.role,
                                                     &document::ParameterBinding::role);
                if (found == node->parameters.end())
                    continue;
                const auto& binding = *found;
                const auto* parameter = composition->parameters().find(binding.parameterId);
                if (!parameter || !(document::isScalarAnimatableSchemaKey(parameter->schemaKey) ||
                                    document::isVec2AnimatableSchemaKey(parameter->schemaKey) ||
                                    document::isColor4AnimatableSchemaKey(parameter->schemaKey) ||
                                    !propertiesSelectorItems(parameter->schemaKey).empty()))
                    continue;
                if (!heading) {
                    group(QObject::tr("Source"));
                    heading = true;
                }
                auto name = node_editor::displayTypeName(binding.role);
                if (binding.role == document::kSolidColorParameterRole ||
                    binding.role == document::kTextColorParameterRole)
                    name = QObject::tr("Color");
                if (binding.role == document::kTextSizeParameterRole)
                    name = QObject::tr("Size");
                if (binding.role == document::kSolidWidthParameterRole)
                    name = QObject::tr("Width");
                if (binding.role == document::kSolidHeightParameterRole)
                    name = QObject::tr("Height");
                if (binding.role == document::kTextAlignmentParameterRole)
                    name = QObject::tr("Alignment");
                if (binding.role == document::kTextLineHeightParameterRole)
                    name = QObject::tr("Line Height");
                if (binding.role == document::kTextLetterSpacingParameterRole)
                    name = QObject::tr("Letter Spacing");
                add(*source, binding.role, name);
            }
        }
        // Task DRIVE-1. A layer's own parameters are not the whole of what animates it: a driven
        // one takes its value from a value node, and that node's keys are what an artist has to
        // reach to change the motion. One collapsible group per node reachable from this layer
        // through driver links, breadth-first and deduplicated, titled by the node's display name
        // -- the same nodes, in the same order, that the Properties panel lists upstream of a
        // selection. The rows inside are ordinary parameter rows, so they carry the same diamond,
        // the same lane, and the same drag gestures every layer parameter already has.
        for (const auto& upstream : session.upstreamNodes(upstreamSeeds(session, layer.layerId),
                                                          UpstreamTraversal::DriverLinksOnly)) {
            const auto* node = composition->graph().findNode(upstream.id);
            const auto* definition =
                node == nullptr
                    ? nullptr
                    : document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
            if (definition == nullptr)
                continue;
            bool heading = false;
            for (const auto& declared : definition->parameters) {
                const auto found = std::ranges::find(node->parameters, declared.role,
                                                     &document::ParameterBinding::role);
                if (found == node->parameters.end())
                    continue;
                const auto* parameter = composition->parameters().find(found->parameterId);
                if (parameter == nullptr || !document::isAnimatableSchemaKey(parameter->schemaKey))
                    continue;
                if (!heading) {
                    group(upstreamGroupKey(upstream.id),
                          node_editor::nodeDisplayName(*composition, *node));
                    heading = true;
                }
                add(upstream.id, found->role, node_editor::displayTypeName(found->role));
            }
        }
    }
    return rows;
}

TimelinePropertyRow::TimelinePropertyRow(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session), label_(new kit::KLabel(this)),
      diamond_(new KeyframeDiamond(session, "", this)), blending_(new kit::KDropdown(this)),
      alignment_(new kit::KDropdown(this)), color_(new kit::KColorChip(this)) {
    setObjectName("timelinePropertyRow");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // task TL-FIX2: explicit zero, not the style-default QHBoxLayout spacing -- this layout now
    // carries two items (the indent spacer below, then the row), and propertyRowIndent()'s exact
    // pixel math assumes nothing but the spacer separates them from this widget's own left edge.
    layout->setSpacing(0);
    // task TL-FIX2: the depth indent, applied fresh on every bind() (this row is pooled and
    // re-bound to entries at different depths as the artist scrolls). Zero at construction; a
    // group/parameter/component entry always sets it before this row is shown.
    indent_ = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout->addSpacerItem(indent_);
    label_->setObjectName("timelinePropertyLabel");
    diamond_->setObjectName("timelinePropertyDiamond");
    auto* indicator = new QWidget(this);
    auto* indicators = new QStackedLayout(indicator);
    indicators->setStackingMode(QStackedLayout::StackAll);
    indicators->setContentsMargins(0, 0, 0, 0);

    disclosure_ = new kit::KIconButton(indicator);
    disclosure_->setObjectName("timelinePropertyDisclosure");
    disclosure_->setFixedSize(kit::px(kit::Size::ToggleCell), kit::px(kit::Size::ToggleCell));
    indicators->addWidget(disclosure_);
    connect(disclosure_, &QToolButton::clicked, this, [this] {
        if (entry_.rowKind == TimelineLayerEntry::Kind::Parameter && toggleParameter)
            toggleParameter(entry_.parameterId);
        else if (toggleGroup)
            toggleGroup(entry_.layerId, entry_.group);
    });
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        auto* cell = cells_[i] = new QWidget(this);
        auto* cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(kit::px(kit::Spacing::XXS));
        auto* component = components_[i] = new kit::KLabel(componentLabel(i), cell);
        component->setObjectName("timelinePropertyComponent");
        component->setFont(kit::font(kit::TypeRole::UiSmall));
        component->hide();
        auto* field = fields_[i] = new kit::KValueField(cell);
        field->setObjectName("timelinePropertyValue");
        field->setCompact(true);
        // task TL-FIX2: no per-component diamond in a parameter row's value cells any more -- the
        // row's own diamond_ (next to the label) is the parameter's single tri-state diamond, and a
        // component's own diamond only exists on its expanded component row (diamond_ again, bound
        // with entry.component set there). Painting one here too was the KEY-2 regression this task
        // fixes: a vector/colour parameter row showed an aggregate diamond AND one per component.
        cell->setFixedWidth(kit::px(kit::Size::PropertiesFieldWidth));
        cellLayout->addWidget(field, 1);

        connect(field, &kit::KValueField::valueChanged, this, [this, i] {
            if (!binding_)
                commitValues(i);
        });
        properties::bindValueEdit(
            session_, *field, [this] { return entry_.parameterId; },
            [this, i]() -> std::optional<document::AnimationComponent> {
                if (entry_.component)
                    return entry_.component;
                if (session_.effectiveColorValue(entry_.parameterId)) {
                    const std::array channels{
                        document::AnimationComponent::Red, document::AnimationComponent::Green,
                        document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
                    return channels[i];
                }
                if (session_.effectiveVec2Value(entry_.parameterId) ||
                    session_.effectiveVec3Value(entry_.parameterId)) {
                    const std::array axes{
                        document::AnimationComponent::X, document::AnimationComponent::Y,
                        document::AnimationComponent::Z, document::AnimationComponent::Z};
                    return axes[i];
                }
                return std::nullopt;
            });
    }
    blending_->setObjectName("timelinePropertyBlending");
    for (auto mode : core::kBlendModes)
        blending_->addItem(blendModeDisplayName(mode),
                           QVariant::fromValue(core::blendModeStoredValue(mode)));

    alignment_->setObjectName("timelinePropertyAlignment");
    alignment_->setAccessibleName(tr("Alignment"));
    alignment_->setControlSize(kit::KDropdown::ControlSize::Compact);

    connect(alignment_, &kit::KDropdown::currentIndexChanged, this, [this](int index) {
        if (!binding_ && index >= 0)
            (void)session_.setParameterValue(entry_.parameterId,
                                             alignment_->itemData(index).value<std::int64_t>(),
                                             tr("Set Parameter"));
    });
    color_->setObjectName("timelinePropertyColor");
    properties::bindValueEdit(session_, *color_, [this] { return entry_.parameterId; });
    connect(&session_, &CompositionSession::liveValueChanged, this, [this] {
        if (!binding_ && entry_.parameterId.isValid())
            bind(entry_);
    });
    // Task DRIVE-1's read-only display for a driven parameter. It occupies the value columns the
    // editors would have, so a driven row is the same row with a different thing in it.
    driven_ = new QWidget(this);
    driven_->setObjectName("timelinePropertyDriven");
    auto* drivenLayout = new QHBoxLayout(driven_);
    drivenLayout->setContentsMargins(0, 0, 0, 0);
    drivenLayout->setSpacing(kit::px(kit::Spacing::XXS));
    driverLink_ = new kit::KButton(driven_);
    driverLink_->setObjectName("timelinePropertyDriverLink");
    driverLink_->setVariant(kit::KButton::Variant::Ghost);
    driverLink_->setControlSize(kit::KButton::ControlSize::Compact);
    driverLink_->setIconId(kit::IconId::Link);
    drivenLayout->addWidget(driverLink_);
    drivenValue_ = new kit::KLabel(driven_);
    drivenValue_->setObjectName("timelinePropertyDrivenValue");
    drivenValue_->setTypeRole(kit::TypeRole::Value);
    drivenValue_->setTextFormat(Qt::PlainText);
    drivenValue_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    drivenLayout->addWidget(drivenValue_, 1);
    driven_->setMinimumWidth(kit::px(kit::Size::ValueCellMin));
    driven_->hide();
    connect(driverLink_, &kit::KButton::clicked, this, [this] {
        if (const auto* driver = session_.driverBindingFor(entry_.parameterId))
            jumpToPropertiesNode(session_, driver->sourceNodeId, this);
    });
    connect(&session_, &CompositionSession::drivenValuesChanged, this, [this] {
        if (isVisible() && session_.driverBindingFor(entry_.parameterId) != nullptr)
            bind(entry_);
    });
    auto* row = new kit::KPropertyRow(label_, indicator,
                                      {diamond_, cells_[0], cells_[1], cells_[2], cells_[3],
                                       blending_, alignment_, color_, driven_},
                                      this, true);
    layout->addWidget(row, 0, Qt::AlignVCenter);
    connect(blending_, &kit::KDropdown::currentIndexChanged, this, [this](int index) {
        if (binding_ || index < 0)
            return;
        const auto mode =
            core::blendModeFromStoredValue(blending_->itemData(index).value<std::int64_t>());
        const auto layer = entry_.layerId;
        session_.selectLayer(layer);
        if (mode)
            (void)session_.setSelectedBlendMode(*mode);
    });
    connect(color_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
        if (binding_)
            return;
        const auto layer = entry_.layerId;
        const auto parameter = entry_.parameterId;
        const auto* composition = session_.composition();
        const auto* record = composition ? composition->parameters().find(parameter) : nullptr;
        const bool solid = record && record->schemaKey == document::kSolidColorParameterSchemaKey;
        const bool text = record && record->schemaKey == document::kTextColorParameterSchemaKey;
        session_.selectLayer(layer);
        if (!record)
            return;
        const auto reference =
            color.converted(kit::ColorSpace::Reference, session_.colorConverter(record->schemaKey));
        if (!reference)
            return;
        const core::Color4d value{
            static_cast<double>(reference->red), static_cast<double>(reference->green),
            static_cast<double>(reference->blue), static_cast<double>(reference->alpha)};
        if (solid)
            (void)session_.setSelectedSolidColor(value);
        else if (text)
            (void)session_.setSelectedTextColor(value);
        else
            (void)session_.setParameterValue(parameter, value, tr("Set Color"));
    });
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] {
        if (isVisible())
            bind(entry_);
    });
}

void TimelinePropertyRow::bind(const TimelineLayerEntry& entry) {
    binding_ = true;
    entry_ = entry;
    setProperty("parameterId",
                QVariant::fromValue(static_cast<qulonglong>(entry.parameterId.value())));
    setProperty("role", QString::fromStdString(entry.role));
    const bool group = entry.rowKind == TimelineLayerEntry::Kind::Group;
    const bool componentRow = entry.component.has_value();
    // task TL-FIX2: nest by depth -- layer 0 (drawn by TimelineLayerRow, never this class), group
    // 1, parameter 2, component 3. The spacer moves the whole indicator+label group right by
    // propertyRowIndent(depth); a component row's label shrinks by exactly the step the spacer grew
    // by since its parameter row, so BOTH land their value columns (diamond_ and the cells) at the
    // same x. A group's own label stays unbounded (below) since it has no value column to align.
    const int depth = std::max(1, entry.depth);
    indent_->changeSize(propertyRowIndent(depth), 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    layout()->invalidate();
    const int labelWidth = componentRow
                               ? kit::px(kit::Size::PropertyLabelCompact) - kPropertyIndentStep
                               : kit::px(kit::Size::PropertyLabelCompact);
    label_->setFixedWidth(labelWidth);
    label_->setMaximumWidth(group ? QWIDGETSIZE_MAX : labelWidth);
    label_->setText(entry.name);
    disclosure_->setVisible(group);
    disclosure_->setIcon(kit::icon(
        entry.expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight, kit::IconRole::Chrome));
    disclosure_->setAccessibleName(tr("Toggle %1").arg(entry.name));
    label_->setFont(kit::font(group ? kit::TypeRole::UiSmall : kit::TypeRole::Ui));
    label_->setToolTip(entry.name);
    setEnabled(true);
    if (group) {
        for (auto* cell : cells_)
            cell->hide();
        blending_->hide();
        alignment_->hide();
        color_->hide();
        diamond_->hide();
        driven_->hide();
    }
    // Task DRIVE-1. A driven parameter has no authored value to edit here: it has a driver, and
    // what the row owes the artist is the driver's name and the value it resolves to right now.
    // The editors below would all show nothing, so none of them is built for this row at all.
    if (!group && session_.driverBindingFor(entry.parameterId) != nullptr) {
        bindDriven(true);
        binding_ = false;
        return;
    }
    bindDriven(false);
    if (!group) {
        // task TL-FIX2: this IS the component's own diamond on an expanded component row (entry.
        // component set) -- the automation name the grammar documents for it is
        // timelineComponentDiamond, distinct from a parameter row's aggregate
        // timelinePropertyDiamond, even though both are painted by this one widget.
        diamond_->setObjectName(componentRow ? QStringLiteral("timelineComponentDiamond")
                                             : QStringLiteral("timelinePropertyDiamond"));
        diamond_->setComponent(entry.component);
        diamond_->setRole(entry.role);
        diamond_->setParameterId(entry.parameterId);
        diamond_->refresh();
        const auto role = std::string_view(entry.role);
        const bool scale = role == document::kScaleParameterRole;
        const bool opacity = role == document::kOpacityParameterRole;
        const bool rotation = role == document::kRotationParameterRole;
        const bool size = role == document::kTextSizeParameterRole;
        const bool dimension = role == document::kSolidWidthParameterRole ||
                               role == document::kSolidHeightParameterRole;
        const bool lineHeight = role == document::kTextLineHeightParameterRole;
        const auto* record = session_.composition()->parameters().find(entry.parameterId);
        const auto items = record ? propertiesSelectorItems(record->schemaKey)
                                  : QList<std::pair<QString, std::int64_t>>{};
        const bool selector = !items.empty() && role != document::kBlendModeParameterRole;
        alignment_->setVisible(selector);
        if (selector) {
            alignment_->clearItems();
            alignment_->setAccessibleName(entry.name);
            for (const auto& [name, stored] : items)
                alignment_->addItem(name, QVariant::fromValue(stored));
            const auto* constant =
                record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
            const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
            if (value)
                alignment_->setCurrentIndex(alignment_->findData(QVariant::fromValue(*value)));
            alignment_->setEnabled(value != nullptr);
            diamond_->hide();
        }
        const auto vector = session_.effectiveVec2Value(entry.parameterId);
        const auto vector3 = session_.effectiveVec3Value(entry.parameterId);
        const auto scalar = session_.effectiveScalarValue(entry.parameterId);
        const auto color = session_.effectiveColorValue(entry.parameterId);
        const bool components = vector.has_value() || vector3.has_value() || color.has_value();
        const int count = componentRow ? 1 : color ? 4 : vector3 ? 3 : vector ? 2 : scalar ? 1 : 0;
        disclosure_->setVisible(components && !componentRow);
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            cells_[i]->setVisible(static_cast<int>(i) < count);
            components_[i]->hide();
            fields_[i]->setLabel(components && !componentRow ? componentLabel(i, color.has_value())
                                                             : QString{});
            fields_[i]->setAccessibleName(
                components ? entry.name + QLatin1Char(' ') + componentLabel(i) : entry.name);
        }
        blending_->setVisible(role == document::kBlendModeParameterRole);
        color_->setVisible(color.has_value() && !componentRow);
        std::array<double, 4> values{};
        if (vector) {
            values[0] = vector->x * (scale ? 100 : 1);
            values[1] = vector->y * (scale ? 100 : 1);
        }
        if (vector3)
            values = {vector3->x, vector3->y, vector3->z, 0.0};
        if (scalar)
            values[0] = *scalar * (opacity ? 100 : 1);
        if (color)
            values = {color->red, color->green, color->blue, color->alpha};
        if (componentRow) {
            const auto component = entry.component.value();
            const auto index =
                static_cast<std::size_t>(component) -
                (color ? static_cast<std::size_t>(document::AnimationComponent::Red) : 0);
            if (index < values.size())
                values[0] = values[index];
        }
        for (int i = 0; i < count; ++i) {
            auto* field = fields_[static_cast<std::size_t>(i)];
            field->setRange(opacity             ? 0
                            : size || dimension ? 1
                            : lineHeight        ? 0.01
                            : scale || rotation ? -100'000
                                                : -1'000'000,
                            opacity             ? 100
                            : size              ? document::kMaximumTextSizePixels
                            : scale || rotation ? 100'000
                                                : 1'000'000);
            field->setDecimals(color ? 3 : opacity || size ? 1 : 2);
            field->setSingleStep(color ? 0.01 : 1);
            field->setUnit(scale || opacity ? "%"
                           : rotation       ? QString::fromUtf8("°")
                           : vector || size || dimension ||
                                   role == document::kTextLetterSpacingParameterRole
                               ? "px"
                               : "");
            // A Vector 3 carries no unit of its own: it is whatever the node reading it means by
            // three numbers, and inventing "px" for one would be a claim nothing supports.
            if (vector3)
                field->setUnit(QString{});
            field->setValue(values[static_cast<std::size_t>(i)]);
            field->show();
        }
        if (role == document::kBlendModeParameterRole) {
            const auto mode = session_.blendModeForLayer(entry.layerId);
            if (mode)
                blending_->setCurrentIndex(static_cast<int>(
                    std::ranges::find(core::kBlendModes, *mode) - core::kBlendModes.begin()));
            blending_->show();
        }
        if (color) {
            const auto converter = session_.colorConverter(record->schemaKey);
            const auto reference = kit::KColor::fromRgba(
                static_cast<float>(color->red), static_cast<float>(color->green),
                static_cast<float>(color->blue), static_cast<float>(color->alpha),
                kit::ColorSpace::Reference);
            color_->setColorConverter(converter);
            color_->setColor(reference);
            color_->setEnabled(
                reference.converted(kit::ColorSpace::Display, converter).has_value());
            color_->setToolTip(exactColorText(*color));
            color_->setVisible(!componentRow);
        }
        const auto* composition = session_.composition();
        const auto* layer = composition ? composition->graph().findLayer(entry.layerId) : nullptr;
        setEnabled(layer && !layer->locked);
    }
    binding_ = false;
}

void TimelinePropertyRow::bindDriven(const bool driven) {
    driven_->setVisible(driven);
    if (!driven)
        return;
    for (auto* cell : cells_)
        cell->hide();
    blending_->hide();
    alignment_->hide();
    color_->hide();
    // A driven parameter carries no curve of its own, so there is no key here to toggle. The
    // driver's OWN keys are reachable: they are the upstream group's rows in this same twirl-down.
    diamond_->hide();
    driverLink_->setText(session_.driverDisplayName(entry_.parameterId));
    driverLink_->setToolTip(tr("Driven by %1").arg(driverLink_->text()));
    driverLink_->setAccessibleName(driverLink_->toolTip());
    const auto resolved = session_.drivenValueText(entry_.parameterId);
    drivenValue_->setText(resolved.isEmpty() ? tr("Resolving…") : resolved);
    drivenValue_->setToolTip(drivenValue_->text());
    setEnabled(true);
    // The value is worker-thread work; asking is idempotent, so asking from the bind that shows
    // the previous answer cannot loop.
    session_.refreshDrivenValues();
}

void TimelinePropertyRow::commitValues(const std::size_t index) {
    const auto entry = entry_;
    const auto role = std::string_view(entry.role);
    const double x = fields_[0]->value(), y = fields_[1]->value(), z = fields_[2]->value();
    if (entry.component) {
        (void)session_.setParameterComponentValue(
            entry.parameterId, *entry.component,
            x / (role == document::kScaleParameterRole ? 100 : 1));
        return;
    }
    const bool color = session_.effectiveColorValue(entry.parameterId).has_value();
    if (color || session_.effectiveVec2Value(entry.parameterId) ||
        session_.effectiveVec3Value(entry.parameterId)) {
        const std::array vectors{document::AnimationComponent::X, document::AnimationComponent::Y,
                                 document::AnimationComponent::Z, document::AnimationComponent::Z};
        const std::array colors{
            document::AnimationComponent::Red, document::AnimationComponent::Green,
            document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
        (void)session_.setParameterComponentValue(
            entry.parameterId, color ? colors[index] : vectors[index],
            fields_[index]->value() / (role == document::kScaleParameterRole ? 100 : 1));
        return;
    }
    session_.selectLayer(entry.layerId);
    if (role == document::kPositionParameterRole)
        (void)session_.setSelectedPosition(x, y);
    else if (role == document::kAnchorParameterRole)
        (void)session_.setSelectedAnchor(x, y);
    else if (role == document::kScaleParameterRole)
        (void)session_.setSelectedScale(x / 100, y / 100);
    else if (role == document::kRotationParameterRole)
        (void)session_.setSelectedRotation(x);
    else if (role == document::kOpacityParameterRole)
        (void)session_.setSelectedOpacity(x / 100);
    else if (role == document::kTextSizeParameterRole)
        (void)session_.setSelectedTextSize(x);
    else if (session_.effectiveVec3Value(entry.parameterId))
        (void)session_.setParameterValue(entry.parameterId, document::Vec3d{x, y, z},
                                         tr("Set Parameter"));
    else if (session_.effectiveVec2Value(entry.parameterId))
        (void)session_.setParameterValue(entry.parameterId, document::Vec2d{x, y},
                                         tr("Set Parameter"));
    else
        (void)session_.setParameterValue(entry.parameterId, x, tr("Set Parameter"));
}
} // namespace bloom::ui
