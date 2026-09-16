#include "properties_registry_row.hpp"
#include "node_editor_items.hpp"
#include "properties_sections.hpp"
#include "properties_value_edits.hpp"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <limits>
#include <memory>

namespace bloom::ui {
document::ShapeKind nodeShapeKind(const document::Composition& composition,
                                  const document::NodeRecord& node) {
    for (const auto& binding : node.parameters)
        if (binding.role == "kind") {
            const auto* p = composition.parameters().find(binding.parameterId);
            const auto* c = p ? std::get_if<document::ConstantValueSource>(&p->source) : nullptr;
            if (const auto* kind = c ? std::get_if<std::int64_t>(&c->value) : nullptr)
                return static_cast<document::ShapeKind>(*kind);
        }
    return document::ShapeKind::Rectangle;
}
PropertiesRowVisibility
propertiesRowVisibility(const std::string_view role, const std::string_view schemaKey,
                        const std::optional<document::ShapeKind> shape) noexcept {
    if (shape && !document::shapeRoleVisible(*shape, role))
        return PropertiesRowVisibility::Hidden;
    // These are presentation metadata, not authored controls. Keep this classification in the UI
    // registry mapping: the document vocabulary remains the durable source of truth, while the
    // Properties surface decides which declared values are meaningful to an artist.
    const bool technicalRole = role == "alpha-association" || role == "color-encoding" ||
                               role == "color-space" || role == "encoding";
    const bool technicalSchema =
        schemaKey == "bloom.color.alpha-association" || schemaKey == "bloom.color.encoding" ||
        schemaKey == "bloom.solid.alpha-association" || schemaKey == "bloom.solid.encoding";
    return technicalRole || technicalSchema ? PropertiesRowVisibility::Hidden
                                            : PropertiesRowVisibility::Visible;
}

PropertiesRegistryRow::PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                                             document::ParameterId parameter,
                                             document::ParameterDefinition definition,
                                             QWidget* parent)
    : QWidget(parent), session_(session), node_(node), parameter_(parameter),
      definition_(std::move(definition)) {
    setObjectName("propertiesRegistryRow");
    setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(parameter.value())));
    setProperty("role", QString::fromStdString(definition_.role));
    const auto label =
        definition_.schemaKey == document::kTextParameterSchemaKey            ? tr("Text")
        : definition_.schemaKey == document::kTextAlignmentParameterSchemaKey ? tr("Text Alignment")
        : definition_.schemaKey == document::kTextLineHeightParameterSchemaKey ? tr("Line Height")
        : definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
            ? tr("Letter Spacing")
        : definition_.schemaKey == document::kTextSizeParameterSchemaKey
            ? tr("Font Size")
            : node_editor::displayTypeName(definition_.role);
    setProperty("rowLabel", label);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(kit::px(kit::Spacing::Gutter));
    auto* controls = new QWidget(this);
    auto* layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    diamond_ = new KeyframeDiamond(session_, definition_.role, this);
    diamond_->setObjectName("propertiesRegistryDiamond");
    diamond_->setParameterId(parameter_);

    const auto items = propertiesSelectorItems(definition_.schemaKey);
    if (definition_.valueKind == document::ParameterValueKind::Path) {
        auto* summary = new kit::KLabel(controls);
        summary->setObjectName("propertiesPathSummary");
        layout->addWidget(summary);
    } else if (definition_.schemaKey == document::kAudioLevelParameterSchemaKey) {
        slider_ = new kit::KSlider(controls);
        slider_->setObjectName(QStringLiteral("propertiesAudioLevelSlider"));
        slider_->setAccessibleName(label);
        slider_->setRange(0.0, 2.0);
        slider_->installEventFilter(this);
        layout->addWidget(slider_);
        connect(slider_, &kit::KSlider::valueChanged, this, [this] { commit(); });
    } else if (propertiesRowControl(definition_.schemaKey) == PropertiesRowControl::SegmentedEnum) {
        segments_ = new kit::KRadioGroup(controls);
        segments_->setObjectName("propertiesRegistryEnum");
        segments_->setAccessibleName(label);
        segments_->setIconsOnly(true);
        const std::array icons{kit::IconId::AlignLeft, kit::IconId::AlignCenter,
                               kit::IconId::AlignRight};
        for (int index = 0; index < items.size(); ++index)
            segments_->addOption(items[index].first, icons[static_cast<std::size_t>(index)]);
        segments_->setFixedSize(segments_->sizeHint());
        layout->addWidget(segments_);
        connect(segments_, &kit::KRadioGroup::currentIndexChanged, this, [this] { commit(); });
    } else if (!items.empty() || definition_.schemaKey == "bloom.image.asset" ||
               definition_.schemaKey == "bloom.audio.asset") {
        selector_ = new kit::KDropdown(controls);
        selector_->setObjectName(
            definition_.schemaKey == "bloom.image.asset"         ? "propertiesImageAsset"
            : definition_.schemaKey == "bloom.audio.asset"       ? "propertiesAudioAsset"
            : definition_.schemaKey == "bloom.image.loop-mode"   ? "propertiesImageLoopMode"
            : definition_.schemaKey == "bloom.image.color-space" ? "propertiesImageColorSpace"
                                                                 : "propertiesRegistryEnum");
        selector_->setControlSize(kit::KDropdown::ControlSize::Compact);
        for (const auto& [name, stored] : items)
            selector_->addItem(name, QVariant::fromValue(stored));
        selector_->setFixedSize(kit::px(kit::Size::PropertiesDropdownWidth),
                                kit::px(kit::Size::ControlCompact));
        layout->addWidget(selector_);
        connect(selector_, &kit::KDropdown::currentIndexChanged, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::Integer) {
        integer_ = new kit::KLineEdit(controls);
        integer_->setObjectName(
            definition_.schemaKey == "bloom.image.start-frame"   ? "propertiesImageStartFrame"
            : definition_.schemaKey == "bloom.audio.start-frame" ? "propertiesAudioStartFrame"
                                                                 : "propertiesRegistryInteger");
        integer_->setAccessibleName(label);
        integer_->setFixedSize(kit::px(kit::Size::PropertiesFieldWidth),
                               kit::px(kit::Size::ControlCompact));
        integer_->setFont(kit::font(kit::TypeRole::Value));
        layout->addWidget(integer_);
        connect(integer_, &QLineEdit::editingFinished, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::Boolean) {
        toggle_ = new kit::KCheckBox(controls);
        toggle_->setObjectName(definition_.schemaKey == "bloom.image.premultiply"
                                   ? "propertiesImagePremultiply"
                                   : "propertiesRegistryBool");
        layout->addWidget(toggle_);
        connect(toggle_, &kit::KSwitch::toggled, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::String) {
        text_ = new kit::KLineEdit(controls);
        text_->setObjectName("propertiesRegistryString");
        text_->setFont(kit::font(kit::TypeRole::Value));
        text_->setFixedHeight(kit::px(kit::Size::ControlCompact));
        layout->addWidget(text_, 1);
        connect(text_, &QLineEdit::editingFinished, this, [this] { commit(); });
        if (definition_.schemaKey == document::kTextParameterSchemaKey) {
            multiline_ = new QPlainTextEdit(this);
            multiline_->setObjectName("propertiesRegistryMultiline");
            multiline_->setFont(kit::font(kit::TypeRole::Value));
            multiline_->setFixedHeight(kit::px(kit::Size::MultilineHeight));
            multiline_->installEventFilter(this);
            multiline_->hide();
            auto* expand = new kit::KButton(controls);
            expand->setObjectName("propertiesRegistryTextExpand");
            expand->setIconId(kit::IconId::CaretRight);
            expand->setVariant(kit::KButton::Variant::Ghost);
            expand->setCheckable(true);
            expand->setFixedSize(kit::px(kit::Size::ControlCompact),
                                 kit::px(kit::Size::ControlCompact));
            expand->setToolTip(tr("Edit multiple lines"));
            layout->addWidget(expand);
            connect(expand, &kit::KButton::toggled, this, [this, expand](bool on) {
                multiline_->setVisible(on);
                expand->setIconId(on ? kit::IconId::CaretDown : kit::IconId::CaretRight);
            });
        }
    } else {
        int count = 1;
        if (definition_.valueKind == document::ParameterValueKind::Vec2d)
            count = 2;
        if (definition_.valueKind == document::ParameterValueKind::Vec3d)
            count = 3;

        if (definition_.valueKind == document::ParameterValueKind::Color4d) {
            count = 4;
            color_ = new kit::KColorChip(controls);
            color_->setObjectName("propertiesRegistryColor");
            properties::bindValueEdit(session_, *color_, [this] { return parameter_; });
            connect(color_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
                if (!refreshing_) {
                    (void)session_.setParameterValue(
                        parameter_,
                        core::Color4d{
                            static_cast<double>(color.red), static_cast<double>(color.green),
                            static_cast<double>(color.blue), static_cast<double>(color.alpha)},
                        tr("Set Color"));
                    refresh();
                }
            });
        }
        for (int i = 0; i < count; ++i) {
            auto* field = fields_[static_cast<std::size_t>(i)] = new kit::KValueField(controls);
            field->setObjectName("propertiesRegistryValue");
            field->setAccessibleName(label);
            field->setRange(-1e15, 1e15);
            field->setCompact(true);
            if (count == 3)
                field->setMinimumWidth(kit::px(kit::Size::PropertiesComponentMinWidth));
            field->setDecimals(2);
            field->setStepper(propertiesRowControl(definition_.schemaKey) ==
                              PropertiesRowControl::Stepper);
            if (definition_.schemaKey == document::kTextLineHeightParameterSchemaKey ||
                definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey)
                field->setUnit("%");
            if (definition_.schemaKey == document::kTextLineHeightParameterSchemaKey ||
                definition_.schemaKey == document::kTextSizeParameterSchemaKey)
                field->setRange(0.01, 1e15);
            if (definition_.schemaKey == document::kSolidWidthParameterSchemaKey ||
                definition_.schemaKey == document::kSolidHeightParameterSchemaKey)
                field->setRange(1, 1e15);
            if (count > 1)
                field->setLabel(QString(color_ ? "RGBA" : "XYZ").mid(i, 1));
            if (count > 1 && !color_) {
                const std::array components{document::AnimationComponent::X,
                                            document::AnimationComponent::Y,
                                            document::AnimationComponent::Z};
                layout->addWidget(properties::makeComponentCell(
                    session_, definition_.role, components[static_cast<std::size_t>(i)], field,
                    controls, parameter_));
            } else
                layout->addWidget(field);
            properties::bindValueEdit(
                session_, *field, [this] { return parameter_; },
                [count, i, this] {
                    if (count == 1)
                        return std::optional<document::AnimationComponent>{};
                    const auto first = color_ ? document::AnimationComponent::Red
                                              : document::AnimationComponent::X;
                    return std::optional(
                        static_cast<document::AnimationComponent>(static_cast<int>(first) + i));
                });
            connect(field, &kit::KValueField::valueChanged, this, [this] { commit(); });
        }
    }
    if (color_) {
        (void)properties::addColorRow(session_, definition_.role, outer, this, color_, diamond_,
                                      {fields_[0], fields_[1], fields_[2], fields_[3]},
                                      "propertiesRegistryColorExpand",
                                      "propertiesRegistryColorFields", parameter_);
        delete controls;
    } else {
        if (!text_ && !multiline_) {
            const int preferred = controls->sizeHint().width();
            controls->setMaximumWidth(preferred);
        }
        properties::addRow(outer, this, properties::makeRowLabel(label, this), diamond_, controls);
    }
    if (multiline_)
        outer->addWidget(multiline_);
    connect(&session_, &CompositionSession::liveValueChanged, this,
            &PropertiesRegistryRow::refresh);
    refresh();
}

bool PropertiesRegistryRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == slider_ && !refreshing_) {
        if (event->type() == QEvent::MouseButtonPress)
            (void)session_.beginValueEdit(parameter_);
        else if (event->type() == QEvent::MouseButtonRelease && session_.isValueEditing(parameter_))
            (void)session_.commitValueEdit();
    }
    if (watched == multiline_ && event->type() == QEvent::FocusOut) {
        text_->setText(multiline_->toPlainText());
        commit();
    }
    return QWidget::eventFilter(watched, event);
}

void PropertiesRegistryRow::refresh() {
    refreshing_ = true;
    const auto* composition = session_.composition();
    const auto* node = composition ? composition->graph().findNode(node_) : nullptr;
    const bool hidden =
        propertiesRowVisibility(definition_.role, definition_.schemaKey,
                                node && node->typeId == document::kShapeSourceNodeType
                                    ? std::optional(nodeShapeKind(*composition, *node))
                                    : std::nullopt) == PropertiesRowVisibility::Hidden;
    setProperty("roleHidden", hidden);
    setVisible(!hidden);
    const auto* parameter = composition ? composition->parameters().find(parameter_) : nullptr;
    const bool editable = parameter && displayScale() != 0.0 && !composition->nodeLocked(node_) &&
                          !std::holds_alternative<document::DriverBindingSource>(parameter->source);
    for (auto* field : fields_)
        if (field) {
            field->setEnabled(editable);
            field->setToolTip(
                displayScale() == 0.0
                    ? tr("Font size must resolve before editing percentage letter spacing")
                    : QString{});
        }
    for (auto* control : {static_cast<QWidget*>(selector_), static_cast<QWidget*>(segments_),
                          static_cast<QWidget*>(toggle_), static_cast<QWidget*>(color_),
                          static_cast<QWidget*>(text_), static_cast<QWidget*>(multiline_),
                          static_cast<QWidget*>(integer_), static_cast<QWidget*>(slider_)})
        if (control)
            control->setEnabled(editable);
    if (parameter) {
        auto value = definition_.defaultValue;
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source))
            value = constant->value;
        if (const auto live = session_.liveValue(parameter_))
            value = *live;
        if (const auto* path = std::get_if<document::PathValue>(&value))
            if (auto* summary = findChild<kit::KLabel*>("propertiesPathSummary"))
                summary->setText(tr("%1 anchors · %2")
                                     .arg(path->anchors.size())
                                     .arg(path->closed ? tr("Closed") : tr("Open")));
        if (const auto scalar = session_.effectiveScalarValue(parameter_))
            value = *scalar;
        if (const auto vector = session_.effectiveVec2Value(parameter_))
            value = *vector;
        if (const auto color = session_.effectiveColorValue(parameter_))
            value = *color;
        if (auto* scalar = std::get_if<double>(&value); scalar && fields_[0])
            if (displayScale() != 0.0)
                fields_[0]->setValue(*scalar * displayScale());
        if (auto* scalar = std::get_if<double>(&value); scalar && slider_)
            slider_->setValue(*scalar);
        if (auto* integer = std::get_if<std::int64_t>(&value)) {
            if (integer_)
                integer_->setText(QString::number(static_cast<qlonglong>(*integer)));
            if (fields_[0])
                fields_[0]->setValue(static_cast<double>(*integer));
            if (segments_)
                segments_->setCurrentIndex(static_cast<int>(*integer));
            if (selector_) {
                for (int index = 0; index < selector_->count(); ++index)
                    if (selector_->itemData(index).value<std::int64_t>() == *integer)
                        selector_->setCurrentIndex(index);
            }
        }
        if (auto* vector = std::get_if<document::Vec2d>(&value); vector && fields_[1]) {
            fields_[0]->setValue(vector->x);
            fields_[1]->setValue(vector->y);
        }
        if (auto* vector = std::get_if<document::Vec3d>(&value); vector && fields_[2]) {
            fields_[0]->setValue(vector->x);
            fields_[1]->setValue(vector->y);
            fields_[2]->setValue(vector->z);
        }
        if (auto* color = std::get_if<core::Color4d>(&value); color && color_) {
            properties::refreshColor(session_, definition_.schemaKey, *color, color_,
                                     {fields_[0], fields_[1], fields_[2], fields_[3]});
        }
        if (auto* boolean = std::get_if<bool>(&value); boolean && toggle_)
            toggle_->setChecked(*boolean);
        if (auto* text = std::get_if<std::string>(&value)) {
            if (selector_ && definition_.schemaKey == "bloom.image.asset")
                refreshImageAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (selector_ && definition_.schemaKey == "bloom.audio.asset")
                refreshAudioAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (text_)
                text_->setText(QString::fromStdString(*text));
            if (multiline_ && !multiline_->hasFocus())
                multiline_->setPlainText(QString::fromStdString(*text));
        }
    }
    diamond_->refresh();
    refreshing_ = false;
}

void PropertiesRegistryRow::reset() {
    resetPropertiesParameter(session_, parameter_);
    refresh();
}

double PropertiesRegistryRow::displayScale() const {
    if (definition_.schemaKey == document::kTextLineHeightParameterSchemaKey)
        return 100.0;
    if (definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey) {
        const auto* composition = session_.composition();
        const auto* node = composition ? composition->graph().findNode(node_) : nullptr;
        if (node)
            for (const auto& binding : node->parameters)
                if (binding.role == document::kTextSizeParameterRole)
                    if (const auto size = session_.effectiveScalarValue(binding.parameterId);
                        size && *size > 0)
                        return 100.0 / *size;
    }
    return definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey ? 0.0 : 1.0;
}

void PropertiesRegistryRow::commit() {
    if (refreshing_ || !isEnabled() || displayScale() == 0.0)
        return;
    document::ParameterValue value = definition_.defaultValue;
    if (segments_)
        value = propertiesSelectorItems(definition_.schemaKey)[segments_->currentIndex()].second;
    else if (selector_ && (definition_.schemaKey == "bloom.image.asset" ||
                           definition_.schemaKey == "bloom.audio.asset"))
        value = selector_->itemData(selector_->currentIndex()).toString().toStdString();
    else if (slider_)
        value = slider_->value();
    else if (selector_)
        value = selector_->itemData(selector_->currentIndex()).value<std::int64_t>();
    else if (integer_) {
        bool valid = false;
        const auto integer = integer_->text().toLongLong(&valid);
        if (!valid) {
            refresh();
            return;
        }
        value = static_cast<std::int64_t>(integer);
    } else if (toggle_)
        value = toggle_->isChecked();
    else if (text_)
        value = text_->text().toStdString();
    else if (multiline_)
        value = multiline_->toPlainText().toStdString();
    else if (color_) {
        const auto converted = properties::colorFromFields(
            session_, definition_.schemaKey, {fields_[0], fields_[1], fields_[2], fields_[3]});
        if (!converted)
            return;
        value = *converted;
    } else if (fields_[2])
        value = document::Vec3d{fields_[0]->value(), fields_[1]->value(), fields_[2]->value()};
    else if (fields_[1])
        value = document::Vec2d{fields_[0]->value(), fields_[1]->value()};
    else if (definition_.valueKind == document::ParameterValueKind::Integer)
        value = static_cast<std::int64_t>(fields_[0]->value());
    else
        value = fields_[0]->value() / displayScale();
    (void)session_.setParameterValue(parameter_, value, tr("Set Parameter"));
    refresh();
}
} // namespace bloom::ui
