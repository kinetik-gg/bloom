#include "timeline_property_rows.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QVariant>
#include <algorithm>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/value_field.hpp>

namespace bloom::ui {
std::vector<TimelineLayerEntry>
timelinePropertyEntries(const CompositionSession& session,
                        const std::vector<TimelineLayerEntry>& layers,
                        const std::set<document::LayerId>& expanded) {
    std::vector<TimelineLayerEntry> rows;
    const auto* composition = session.composition();
    if (!composition)
        return rows;
    for (auto layer : layers) {
        layer.expanded = expanded.contains(layer.layerId);
        rows.push_back(layer);
        if (!layer.expanded)
            continue;
        const auto group = [&](const QString& name) {
            auto entry = layer;
            entry.rowKind = TimelineLayerEntry::Kind::Group;
            entry.name = name;
            rows.push_back(entry);
        };
        const auto add = [&](document::NodeId nodeId, std::string_view role, const QString& name) {
            const auto* node = composition->graph().findNode(nodeId);
            if (!node)
                return;
            for (const auto& binding : node->parameters) {
                if (binding.role != role)
                    continue;
                auto entry = layer;
                entry.rowKind = TimelineLayerEntry::Kind::Parameter;
                entry.name = name;
                entry.role = binding.role;
                entry.parameterId = binding.parameterId;
                rows.push_back(entry);
            }
        };
        if (const auto boundary = session.boundaryNodeForLayer(layer.layerId)) {
            group(QObject::tr("Transform"));
            add(*boundary, document::kPositionParameterRole, QObject::tr("Position"));
            add(*boundary, document::kAnchorParameterRole, QObject::tr("Anchor"));
            add(*boundary, document::kScaleParameterRole, QObject::tr("Scale"));
            add(*boundary, document::kRotationParameterRole, QObject::tr("Rotation"));
            group(QObject::tr("Appearance"));
            add(*boundary, document::kOpacityParameterRole, QObject::tr("Opacity"));
            add(*boundary, document::kBlendModeParameterRole, QObject::tr("Blending"));
        }
        if (const auto source = session.directSourceNodeForLayer(layer.layerId)) {
            const auto* node = composition->graph().findNode(*source);
            bool heading = false;
            for (const auto& binding : node->parameters) {
                if (session.keyframeDiamondStateForParameter(binding.parameterId) ==
                    KeyframeDiamondState::Unsupported)
                    continue;
                if (!heading) {
                    group(QObject::tr("Source"));
                    heading = true;
                }
                auto name = QString::fromStdString(binding.role);
                if (binding.role == document::kSolidColorParameterRole ||
                    binding.role == document::kTextColorParameterRole)
                    name = QObject::tr("Color");
                if (binding.role == document::kTextSizeParameterRole)
                    name = QObject::tr("Size");
                add(*source, binding.role, name);
            }
        }
    }
    return rows;
}

TimelinePropertyRow::TimelinePropertyRow(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session), label_(new QLabel(this)),
      diamond_(new KeyframeDiamond(session, "", this)), blending_(new kit::KDropdown(this)),
      color_(new kit::KColorChip(this)) {
    setObjectName("timelinePropertyRow");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::L), 0, kit::px(kit::Spacing::XS), 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    label_->setObjectName("timelinePropertyLabel");
    label_->setFixedWidth(kit::px(kit::Size::ControlRoomy) * 3);
    label_->setFont(kit::font(kit::TypeRole::Ui));
    layout->addWidget(label_);
    diamond_->setObjectName("timelinePropertyDiamond");
    layout->addWidget(diamond_);
    for (auto*& field : fields_) {
        field = new kit::KValueField(this);
        field->setObjectName("timelinePropertyValue");
        field->setMinimumWidth(0);
        layout->addWidget(field, 1);
        connect(field, &kit::KValueField::valueChanged, this, [this] {
            if (!binding_)
                commitValues();
        });
    }
    blending_->setObjectName("timelinePropertyBlending");
    for (auto mode : core::kBlendModes)
        blending_->addItem(blendModeDisplayName(mode),
                           QVariant::fromValue(core::blendModeStoredValue(mode)));
    layout->addWidget(blending_, 1);
    color_->setObjectName("timelinePropertyColor");
    layout->addWidget(color_, 1);
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
        session_.selectLayer(layer);
        (void)session_.setSelectedTextColor(
            {static_cast<double>(color.red), static_cast<double>(color.green),
             static_cast<double>(color.blue), static_cast<double>(color.alpha)});
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
    label_->setText(group ? entry.name.toUpper() : entry.name);
    label_->setToolTip(entry.name);
    setEnabled(true);
    if (group) {
        for (auto* field : fields_)
            field->hide();
        blending_->hide();
        color_->hide();
        diamond_->hide();
    }
    if (!group) {
        diamond_->setParameterId(entry.parameterId);
        diamond_->refresh();
        const auto role = std::string_view(entry.role);
        const bool scale = role == document::kScaleParameterRole;
        const bool opacity = role == document::kOpacityParameterRole;
        const bool rotation = role == document::kRotationParameterRole;
        const bool size = role == document::kTextSizeParameterRole;
        const auto vector = session_.effectiveVec2Value(entry.parameterId);
        const auto scalar = session_.effectiveScalarValue(entry.parameterId);
        const auto color = session_.effectiveColorValue(entry.parameterId);
        const int count = vector                                                ? 2
                          : scalar                                              ? 1
                          : color && role == document::kSolidColorParameterRole ? 4
                                                                                : 0;
        for (std::size_t i = 0; i < fields_.size(); ++i)
            fields_[i]->setVisible(static_cast<int>(i) < count);
        blending_->setVisible(role == document::kBlendModeParameterRole);
        color_->setVisible(color.has_value() && role == document::kTextColorParameterRole);
        std::array<double, 4> values{};
        if (vector) {
            values[0] = vector->x * (scale ? 100 : 1);
            values[1] = vector->y * (scale ? 100 : 1);
        }
        if (scalar)
            values[0] = *scalar * (opacity ? 100 : 1);
        if (color)
            values = {color->red, color->green, color->blue, color->alpha};
        for (int i = 0; i < count; ++i) {
            auto* field = fields_[static_cast<std::size_t>(i)];
            field->setRange(opacity             ? 0
                            : size              ? 1
                            : scale || rotation ? -100'000
                                                : -1'000'000,
                            opacity             ? 100
                            : size              ? document::kMaximumTextSizePixels
                            : scale || rotation ? 100'000
                                                : 1'000'000);
            field->setDecimals(color ? 3 : opacity || size ? 1 : 2);
            field->setSingleStep(color ? 0.01 : 1);
            field->setLabel(vector  ? (i == 0 ? "X" : "Y")
                            : color ? QString("RGBA").mid(i, 1)
                                    : QString());
            field->setUnit(scale || opacity ? "%"
                           : rotation       ? QString::fromUtf8("°")
                           : vector || size ? "px"
                                            : "");
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
        if (color && role == document::kTextColorParameterRole) {
            color_->setColor({static_cast<float>(color->red), static_cast<float>(color->green),
                              static_cast<float>(color->blue), static_cast<float>(color->alpha)});
            color_->setToolTip(exactColorText(*color));
            color_->show();
        }
        const auto* composition = session_.composition();
        const auto* layer = composition ? composition->graph().findLayer(entry.layerId) : nullptr;
        setEnabled(layer && !layer->locked);
    }
    binding_ = false;
}

void TimelinePropertyRow::commitValues() {
    const auto entry = entry_;
    const auto role = std::string_view(entry.role);
    const double x = fields_[0]->value(), y = fields_[1]->value();
    const core::Color4d color{x, y, fields_[2]->value(), fields_[3]->value()};
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
    else if (role == document::kSolidColorParameterRole)
        (void)session_.setSelectedSolidColor(color);
    else if (role == document::kTextSizeParameterRole)
        (void)session_.setSelectedTextSize(x);
}
} // namespace bloom::ui
