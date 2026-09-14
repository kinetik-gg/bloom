#include "properties_registry_row.hpp"
#include "node_editor_items.hpp"
#include "properties_sections.hpp"
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
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <limits>

namespace bloom::ui {
PropertiesRegistryRow::PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                                             document::ParameterId parameter,
                                             document::ParameterDefinition definition,
                                             QWidget* parent)
    : QWidget(parent), session_(session), node_(node), parameter_(parameter),
      definition_(std::move(definition)) {
    setObjectName("propertiesRegistryRow");
    setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(parameter.value())));
    setProperty("role", QString::fromStdString(definition_.role));
    const auto label = node_editor::displayTypeName(definition_.role);
    setProperty("rowLabel", label);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* controls = new QWidget(this);
    auto* layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    diamond_ = new KeyframeDiamond(session_, definition_.role, this);
    diamond_->setObjectName("propertiesRegistryDiamond");
    diamond_->setParameterId(parameter_);
    properties::addRow(outer, this, properties::makeRowLabel(label, this), diamond_, controls);
    const auto items = propertiesSelectorItems(definition_.schemaKey);
    if (!items.empty()) {
        selector_ = new kit::KDropdown(controls);
        selector_->setObjectName("propertiesRegistryEnum");
        for (const auto& [name, stored] : items)
            selector_->addItem(name, QVariant::fromValue(stored));
        layout->addWidget(selector_, 1);
        connect(selector_, &kit::KDropdown::currentIndexChanged, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::Boolean) {
        toggle_ = new kit::KSwitch(controls);
        toggle_->setObjectName("propertiesRegistryBool");
        layout->addWidget(toggle_);
        connect(toggle_, &kit::KSwitch::toggled, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::String) {
        if (definition_.schemaKey == document::kTextParameterSchemaKey) {
            multiline_ = new QPlainTextEdit(controls);
            multiline_->setObjectName("propertiesRegistryMultiline");
            multiline_->setFixedHeight(kit::px(kit::Size::Control) * 3);
            multiline_->installEventFilter(this);
            layout->addWidget(multiline_, 1);
        } else {
            text_ = new QLineEdit(controls);
            text_->setObjectName("propertiesRegistryString");
            layout->addWidget(text_, 1);
            connect(text_, &QLineEdit::editingFinished, this, [this] { commit(); });
        }
    } else {
        int count = 1;
        if (definition_.valueKind == document::ParameterValueKind::Vec2d)
            count = 2;
        if (definition_.valueKind == document::ParameterValueKind::Vec3d)
            count = 3;
        QVBoxLayout* channels = nullptr;
        if (definition_.valueKind == document::ParameterValueKind::Color4d) {
            count = 4;
            color_ = new kit::KColorChip(controls);
            color_->setObjectName("propertiesRegistryColor");
            layout->addWidget(color_);
            auto* expand = new kit::KButton(controls);
            expand->setObjectName("propertiesRegistryColorExpand");
            expand->setText(tr("RGBA"));
            expand->setCheckable(true);
            layout->addWidget(expand);
            auto* details = new QWidget(this);
            channels = new QVBoxLayout(details);
            channels->setContentsMargins(0, 0, 0, 0);
            outer->addWidget(details);
            details->hide();
            connect(expand, &kit::KButton::toggled, details, &QWidget::setVisible);
            connect(color_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
                if (!refreshing_) {
                    (void)session_.setParameterValue(
                        parameter_, core::Color4d{color.red, color.green, color.blue, color.alpha},
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
            field->setDecimals(definition_.valueKind == document::ParameterValueKind::Integer ? 0
                                                                                              : 3);
            if (channels) {
                properties::addRow(channels, this,
                                   properties::makeRowLabel(QString("RGBA").mid(i, 1), this),
                                   nullptr, field);
            } else
                layout->addWidget(field, 1);
            connect(field, &kit::KValueField::valueChanged, this, [this] {
                if (!scrubbing_)
                    commit();
            });
            connect(field, &kit::KValueField::scrubStarted, this, [this] { scrubbing_ = true; });
            connect(field, &kit::KValueField::scrubCancelled, this, [this] { scrubbing_ = false; });
            connect(field, &kit::KValueField::scrubFinished, this, [this] {
                scrubbing_ = false;
                commit();
            });
        }
    }
    refresh();
}

bool PropertiesRegistryRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == multiline_ && event->type() == QEvent::FocusOut)
        commit();
    return QWidget::eventFilter(watched, event);
}

void PropertiesRegistryRow::refresh() {
    refreshing_ = true;
    const auto* composition = session_.composition();
    const auto* parameter = composition ? composition->parameters().find(parameter_) : nullptr;
    setEnabled(parameter && !composition->nodeLocked(node_) &&
               !std::holds_alternative<document::DriverBindingSource>(parameter->source));
    if (parameter) {
        auto value = definition_.defaultValue;
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source))
            value = constant->value;
        if (const auto scalar = session_.effectiveScalarValue(parameter_))
            value = *scalar;
        if (const auto vector = session_.effectiveVec2Value(parameter_))
            value = *vector;
        if (const auto color = session_.effectiveColorValue(parameter_))
            value = *color;
        if (auto* scalar = std::get_if<double>(&value); scalar && fields_[0])
            fields_[0]->setValue(*scalar);
        if (auto* integer = std::get_if<std::int64_t>(&value)) {
            if (fields_[0])
                fields_[0]->setValue(static_cast<double>(*integer));
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
            color_->setColor({static_cast<float>(color->red), static_cast<float>(color->green),
                              static_cast<float>(color->blue), static_cast<float>(color->alpha)});
            const std::array channels{color->red, color->green, color->blue, color->alpha};
            for (std::size_t i = 0; i < channels.size(); ++i)
                fields_[i]->setValue(channels[i]);
        }
        if (auto* boolean = std::get_if<bool>(&value); boolean && toggle_)
            toggle_->setChecked(*boolean);
        if (auto* text = std::get_if<std::string>(&value)) {
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
    (void)session_.setParameterValue(parameter_, definition_.defaultValue, tr("Reset Parameter"));
    refresh();
}

void PropertiesRegistryRow::commit() {
    if (refreshing_ || !isEnabled())
        return;
    document::ParameterValue value = definition_.defaultValue;
    if (selector_)
        value = selector_->itemData(selector_->currentIndex()).value<std::int64_t>();
    else if (toggle_)
        value = toggle_->isChecked();
    else if (text_)
        value = text_->text().toStdString();
    else if (multiline_)
        value = multiline_->toPlainText().toStdString();
    else if (color_)
        value = core::Color4d{fields_[0]->value(), fields_[1]->value(), fields_[2]->value(),
                              fields_[3]->value()};
    else if (fields_[2])
        value = document::Vec3d{fields_[0]->value(), fields_[1]->value(), fields_[2]->value()};
    else if (fields_[1])
        value = document::Vec2d{fields_[0]->value(), fields_[1]->value()};
    else if (definition_.valueKind == document::ParameterValueKind::Integer)
        value = static_cast<std::int64_t>(fields_[0]->value());
    else
        value = fields_[0]->value();
    (void)session_.setParameterValue(parameter_, value, tr("Set Parameter"));
    refresh();
}
} // namespace bloom::ui
